#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "myMalloc.h"
#include "printing.h"

/* Due to the way assert() prints error messges we use out own assert function
 * for deteminism when testing assertions
 */
#ifdef TEST_ASSERT
  inline static void assert(int e) {
    if (!e) {
      const char * msg = "Assertion Failed!\n";
      write(2, msg, strlen(msg));
      exit(1);
    }
  }
#else
  #include <assert.h>
#endif

/*
 * Mutex to ensure thread safety for the freelist
 */
static pthread_mutex_t mutex;

/*
 * Array of sentinel nodes for the freelists
 */
header freelistSentinels[N_LISTS];

/*
 * Pointer to the second fencepost in the most recently allocated chunk from
 * the OS. Used for coalescing chunks
 */
header * lastFencePost;

/*
 * Pointer to maintian the base of the heap to allow printing based on the
 * distance from the base of the heap
 */ 
void * base;

/*
 * List of chunks allocated by  the OS for printing boundary tags
 */
header * osChunkList [MAX_OS_CHUNKS];
size_t numOsChunks = 0;

/*
 * direct the compiler to run the init function before running main
 * this allows initialization of required globals
 */
static void init (void) __attribute__ ((constructor));

// Helper functions for manipulating pointers to headers
static inline header * get_header_from_offset(void * ptr, ptrdiff_t off);
static inline header * get_left_header(header * h);
static inline header * ptr_to_header(void * p);

// Helper functions for allocating more memory from the OS
static inline void initialize_fencepost(header * fp, size_t left_size);
static inline void insert_os_chunk(header * hdr);
static inline void insert_fenceposts(void * raw_mem, size_t size);
static header * allocate_chunk(size_t size);

// Helper functions for freeing a block
static inline void deallocate_object(void * p);

// Helper functions for allocating a block
static inline header * allocate_object(size_t raw_size);

// Helper functions for verifying that the data structures are structurally 
// valid
static inline header * detect_cycles();
static inline header * verify_pointers();
static inline bool verify_freelist();
static inline header * verify_chunk(header * chunk);
static inline bool verify_tags();
static int index_to_insert(size_t data_size);
static void init();
static void coalesce(header *a, header *b);
static void remove_block_from_list(header *remove);
static bool isMallocInitialized;
static void allocate_new_chunk();
/**
 * @brief Helper function to retrieve a header pointer from a pointer and an 
 *        offset
 *
 * @param ptr base pointer
 * @param off number of bytes from base pointer where header is located
 *
 * @return a pointer to a header offset bytes from pointer
 */
static inline header * get_header_from_offset(void * ptr, ptrdiff_t off) {
	return (header *)((char *) ptr + off);
}

/**
 * @brief Helper function to get the header to the right of a given header
 *
 * @param h original header
 *
 * @return header to the right of h
 */
header * get_right_header(header * h) {
	return get_header_from_offset(h, get_block_size(h));
}

/**
 * @brief Helper function to get the header to the left of a given header
 *
 * @param h original header
 *
 * @return header to the right of h
 * @https://www.google.com*/
inline static header * get_left_header(header * h) {
  return get_header_from_offset(h, -h->left_size);
}

/**
 * @brief Fenceposts are marked as always allocated and may need to have
 * a left object size to ensure coalescing happens properly
 *
 * @param fp a pointer to the header being used as a fencepost
 * @param left_size the size of the object to the left of the fencepost
 */
inline static void initialize_fencepost(header * fp, size_t left_size) {
	set_block_state(fp,FENCEPOST);
	set_block_size(fp, ALLOC_HEADER_SIZE);
	fp->left_size = left_size;
}

/**
 * @brief Helper function to maintain list of chunks from the OS for debugging
 *
 * @param hdr the first fencepost in the chunk allocated by the OS
 */
inline static void insert_os_chunk(header * hdr) {
  if (numOsChunks < MAX_OS_CHUNKS) {
    osChunkList[numOsChunks++] = hdr;
  }
}

/**
 * @brief given a chunk of memory insert fenceposts at the left and 
 * right boundaries of the block to prevent coalescing outside of the
 * block
 *
 * @param raw_mem a void pointer to the memory chunk to initialize
 * @param size the size of the allocated chunk
 */
inline static void insert_fenceposts(void * raw_mem, size_t size) {
  // Convert to char * before performing operations
  char * mem = (char *) raw_mem;

  // Insert a fencepost at the left edge of the block
  header * leftFencePost = (header *) mem;
  initialize_fencepost(leftFencePost, ALLOC_HEADER_SIZE);

  // Insert a fencepost at the right edge of the block
  header * rightFencePost = get_header_from_offset(mem, size - ALLOC_HEADER_SIZE);
  initialize_fencepost(rightFencePost, size - 2 * ALLOC_HEADER_SIZE);
}

/**
 * @brief Allocate another chunk from the OS and prepare to insert it
 * into the free list
 *
 * @param size The size to allocate from the OS
 *
 * @return A pointer to the allocable block in the chunk (just after the 
 * first fencpost)
 */
static header * allocate_chunk(size_t size) {
  void * mem = sbrk(size);
  
  insert_fenceposts(mem, size);
  header * hdr = (header *) ((char *)mem + ALLOC_HEADER_SIZE);
  set_block_state(hdr, UNALLOCATED);
  set_block_size(hdr, size - 2 * ALLOC_HEADER_SIZE);
  hdr->left_size = ALLOC_HEADER_SIZE;
  return hdr;
}


static size_t round_size(size_t raw_size) {
  if(raw_size + ALLOC_HEADER_SIZE < 2*ALLOC_HEADER_SIZE){  //ALLOC_HEADER_SIZE
    return 2*ALLOC_HEADER_SIZE;
  }
  else {
    if(raw_size%8 == 0) {
      return raw_size+ALLOC_HEADER_SIZE;
    }
    else {
      return(((raw_size/8)+1)*8)+ALLOC_HEADER_SIZE;
    }
  }
}

static int get_free_list_index(size_t data_size) {
  int starting_index = (data_size-ALLOC_HEADER_SIZE)/8 - 1;
  for(int i = starting_index; i < N_LISTS-1; i++) {
    if((header*)(&(freelistSentinels[i]))->next != (header*)(&(freelistSentinels[i]))) {
      return i;
    }
  }
  return N_LISTS - 1;
}

static header *find_proper_block(size_t data_size) {
  int index = get_free_list_index(data_size);
  if(index < N_LISTS - 1) {
    return (header*)(&(freelistSentinels[index]))->next;
  }
  else {
    header *sentinel = (header*)&(freelistSentinels[N_LISTS-1]);
    header *temp = sentinel->next;
    while(temp != sentinel) {
      if(get_block_size(temp) >= data_size) {
        return temp;
      }
      temp = temp->next;
    }
    return NULL;
  }
}
//static int isExactSize(int index, size_t data_size) {
  //return(index == (data_size-16)/8 -1);
//}

static int can_split(header *h, size_t data_size) {
  return((get_block_size(h)-2*ALLOC_HEADER_SIZE) >= (data_size));
}

static header* remove_block(int index) {
  header *beginning = (&freelistSentinels[index]);
  header *returns = beginning->next;
  header *right = returns->next;
  right->prev = beginning;
  beginning->next = right;
  return returns;
}
static void remove_block_from_list(header *remove) {
  header *left = remove->prev;
  header *right = remove->next;
  left->next = right;
  right->prev = left;
  remove->prev = NULL;
  remove->next = NULL;
}

static int index_to_insert(size_t data_size) {
  int index = ((data_size-ALLOC_HEADER_SIZE)/8-1);
  if(index < N_LISTS-1) {
    return index;
  }
  return N_LISTS-1;
}
static void insert_block(header *block) {
  size_t size = get_block_size(block);
  int index = index_to_insert(size);
  header *sentinel = &(freelistSentinels[index]);
  header *second = sentinel->next;
  sentinel->next = block;
  block->prev = sentinel;
  block->next = second;
  second->prev = block;
}

static void reinsert_block(header *h, header *left, header *right) {
   left->next = h;
   h->prev = left;
   h->next = right;
   right->prev = h;
}

static header *remove_and_split_block(size_t data_size) {
  header *block = find_proper_block(data_size);
  int oldindex = index_to_insert(get_block_size(block));
  header *prev = block->prev;
  header *next = block->next;
  remove_block_from_list(block);
  size_t block_size = get_block_size(block);
  size_t remaining_size = block_size - data_size;
  set_block_size(block,remaining_size);
  header *returning = get_right_header(block);
  set_block_size_and_state(returning,data_size,ALLOCATED);
  returning->left_size = remaining_size;
  header *right = get_right_header(returning);
  right->left_size = data_size;
  int new_index = index_to_insert(get_block_size(block));
  if(oldindex == new_index) {
    reinsert_block(block,prev,next);
  }
  else {
    insert_block(block);
  }
  return (header*)(returning);
}

/**
 * @brief Helper allocate an object given a raw request size from the user
 *
 * @param raw_size number of bytes the user needs
 *
 * @return A block satisfying the user's request
 */
static inline header * allocate_object(size_t raw_size) {
  // TODO implement allocation
  if(raw_size == 0) {
    return NULL;
  }
  size_t new_size = round_size(raw_size);
  int index = get_free_list_index(new_size);
  header *block = find_proper_block(new_size);
  if(block != NULL && !(can_split(block,new_size))) {
    header *temp = block;
    remove_block_from_list(temp);
    set_block_state(temp,ALLOCATED);
    return (header*)temp->data;
  }
  else {
    while(find_proper_block(new_size) == NULL) {
      allocate_new_chunk();
    }
    if(!(can_split(find_proper_block(new_size),new_size))) {
       header *temp = find_proper_block(new_size);
       remove_block_from_list(temp);
       set_block_state(temp,ALLOCATED);
       return (header*)temp->data;
    }
    return(header*)(remove_and_split_block(new_size)->data);
  }
}

/**
 * @brief Helper to get the header from a pointer allocated with malloc
 *
 * @param p pointer to the data region of the block
 *
 * @return A pointer to the header of the block
 */
static void allocate_new_chunk() {
  header *last = lastFencePost;
  header *new_chunk = allocate_chunk(ARENA_SIZE);
  header *fencepost = get_left_header(new_chunk);
  lastFencePost = get_right_header(new_chunk);
  if(fencepost == get_right_header(last)) {
    if(get_block_state(get_left_header(last)) != UNALLOCATED) {
      set_block_size_and_state(last, 2*get_block_size(last) + get_block_size(new_chunk),UNALLOCATED);
      lastFencePost->left_size = get_block_size(last);
      insert_block(last);
    }
    else {
      header *old_chunk = get_left_header(last);
      int old_size = index_to_insert(get_block_size(old_chunk));
      set_block_size(old_chunk, get_block_size(old_chunk) + 2 * get_block_size(last) + get_block_size(new_chunk));
      lastFencePost->left_size = get_block_size(old_chunk);
      int new_size = index_to_insert(get_block_size(old_chunk));
      if(old_size != new_size) {
        remove_block_from_list(old_chunk);
        insert_block(old_chunk);
      }
    }
  }
  else {
    insert_os_chunk(fencepost);
    insert_block(new_chunk);
  }
}

static inline header * ptr_to_header(void * p) {
  return (header *)((char *) p - ALLOC_HEADER_SIZE); //sizeof(header));
}

/**
 * @brief Helper to manage deallocation of a pointer returned by the user
 *
 * @param p The pointer returned to the user by a call to malloc
 */
static inline void deallocate_object(void * p) {
  if(p == NULL) {
    return;
  }
  if(get_block_state((header*)(p-16)) == UNALLOCATED) {
    printf("Double Free Detected\n");
    assert(0);
    return;
  }
  header *block = ptr_to_header(p);
  set_block_state(block,UNALLOCATED);
  if(get_block_state(get_left_header(block)) != UNALLOCATED && get_block_state(get_right_header(block)) != UNALLOCATED) {
    insert_block(block);
  }
  else if(get_block_state(get_left_header(block)) == UNALLOCATED && (get_block_state(get_right_header(block)) != UNALLOCATED)) {
    header *right = get_right_header(block);
    header *final = get_left_header(block);
    int old_index = index_to_insert(get_block_size(final));
    coalesce(final,block);
    int new_index = index_to_insert(get_block_size(final));
    if(new_index != old_index) {
      remove_block_from_list(final);
      insert_block(final);
    }
    right->left_size = get_block_size(final);
  }
  else if(get_block_state(get_left_header(block)) != UNALLOCATED && (get_block_state(get_right_header(block)) == UNALLOCATED)) {
    header *right = get_right_header(block);
    header *prev = right->prev;
    header *next = right->next;
    remove_block_from_list(right);
    int old_index = index_to_insert(get_block_size(right));
    coalesce(block, right);
    get_right_header(block)->left_size = get_block_size(block);
    int new_index = index_to_insert(get_block_size(block));
    if(old_index != new_index) {
      insert_block(block);
    }
    else {
    reinsert_block(block,prev,next);
   }
 }
 else if(get_block_state(get_left_header(block)) == UNALLOCATED && get_block_state(get_right_header(block)) == UNALLOCATED) {
   header *left = get_left_header(block);
   header *right = get_right_header(block);
   header *prev = left->prev;
   header *next = left->next;
   int old_index = index_to_insert(get_block_size(left));
   remove_block_from_list(right);
   remove_block_from_list(left);
   coalesce(left,block);
   coalesce(left,right);
   get_right_header(left)->left_size = get_block_size(left);
   int new_index = index_to_insert(get_block_size(left));
   if(old_index != new_index) {
     insert_block(left);
   }
   else {
      reinsert_block(left,prev,next);
   }
 }

}

static void coalesce(header *a, header *b) {
  set_block_size(a, get_block_size(a) + get_block_size(b));
}

/**
 * @brief Helper to detect cycles in the free list
 * https://en.wikipedia.org/wiki/Cycle_detection#Floyd's_Tortoise_and_Hare
 *
 * @return One of the nodes in the cycle or NULL if no cycle is present
 */
static inline header * detect_cycles() {
  for (int i = 0; i < N_LISTS; i++) {
    header * freelist = &freelistSentinels[i];
    for (header * slow = freelist->next, * fast = freelist->next->next; 
         fast != freelist; 
         slow = slow->next, fast = fast->next->next) {
      if (slow == fast) {
        return slow;
      }
    }
  }
  return NULL;
}

/**
 * @brief Helper to verify that there are no unlinked previous or next pointers
 *        in the free list
 *
 * @return A node whose previous and next pointers are incorrect or NULL if no
 *         such node exists
 */
static inline header * verify_pointers() {
  for (int i = 0; i < N_LISTS; i++) {
    header * freelist = &freelistSentinels[i];
    for (header * cur = freelist->next; cur != freelist; cur = cur->next) {
      if (cur->next->prev != cur || cur->prev->next != cur) {
        return cur;
      }
    }
  }
  return NULL;
}

/**
 * @brief Verify the structure of the free list is correct by checkin for 
 *        cycles and misdirected pointers
 *
 * @return true if the list is valid
 */
static inline bool verify_freelist() {
  header * cycle = detect_cycles();
  if (cycle != NULL) {
    fprintf(stderr, "Cycle Detected\n");
    print_sublist(print_object, cycle->next, cycle);
    return false;
  }
  header * invalid = verify_pointers();
  if (invalid != NULL) {
    fprintf(stderr, "Invalid pointers\n");
    print_object(invalid);
    return false;
  }

  return true;
}

/**
 * @brief Helper to verify that the sizes in a chunk from the OS are correct
 *        and that allocated node's canary values are correct
 *
 * @param chunk AREA_SIZE chunk allocated from the OS
 *
 * @return a pointer to an invalid header or NULL if all header's are valid
 */
static inline header * verify_chunk(header * chunk) {
	if (get_block_state(chunk) != FENCEPOST) {
		fprintf(stderr, "Invalid fencepost\n");
		print_object(chunk);
		return chunk;
	}
	
	for (; get_block_state(chunk) != FENCEPOST; chunk = get_right_header(chunk)) {
		if (get_block_size(chunk)  != get_right_header(chunk)->left_size) {
			fprintf(stderr, "Invalid sizes\n");
			print_object(chunk);
			return chunk;
		}
	}
	
	return NULL;
}

/**
 * @brief For each chunk allocated by the OS verify that the boundary tags
 *        are consistent
 *
 * @return true if the boundary tags are valid
 */
static inline bool verify_tags() {
  for (size_t i = 0; i < numOsChunks; i++) {
    header * invalid = verify_chunk(osChunkList[i]);
    if (invalid != NULL) {
      return invalid;
    }
  }

  return NULL;
}

/**
 * @brief Initialize mutex lock and prepare an 
 initial chunk of memory for allocation
 */
static void init() {
  // Initialize mutex for thread safety
  pthread_mutex_init(&mutex, NULL);

#ifdef DEBUG
  // Manually set printf buffer so it won't call malloc when debugging the allocator
  setvbuf(stdout, NULL, _IONBF, 0);
#endif // DEBUG

  // Allocate the first chunk from the OS
  header * block = allocate_chunk(ARENA_SIZE);

  header * prevFencePost = get_header_from_offset(block, -ALLOC_HEADER_SIZE);
  insert_os_chunk(prevFencePost);

  lastFencePost = get_header_from_offset(block, get_block_size(block));

  // Set the base pointer to the beginning of the first fencepost in the first
  // chunk from the OS
  base = ((char *) block) - ALLOC_HEADER_SIZE; //sizeof(header);

  // Initialize freelist sentinels
  for (int i = 0; i < N_LISTS; i++) {
    header * freelist = &freelistSentinels[i];
    freelist->next = freelist;
    freelist->prev = freelist;
  }

  // Insert first chunk into the free list
  header * freelist = &freelistSentinels[N_LISTS - 1];
  freelist->next = block;
  freelist->prev = block;
  block->next = freelist;
  block->prev = freelist;
}

/* 
 * External interface
 */
void * my_malloc(size_t size) {
  pthread_mutex_lock(&mutex);
  header * hdr = allocate_object(size); 
  pthread_mutex_unlock(&mutex);
  return hdr;
}

void * my_calloc(size_t nmemb, size_t size) {
  return memset(my_malloc(size * nmemb), 0, size * nmemb);
}

void * my_realloc(void * ptr, size_t size) {
  void * mem = my_malloc(size);
  memcpy(mem, ptr, size);
  my_free(ptr);
  return mem; 
}

void my_free(void * p) {
  pthread_mutex_lock(&mutex);
  deallocate_object(p);
  pthread_mutex_unlock(&mutex);
}

bool verify() {
  return verify_freelist() && verify_tags();
}
