# -*- coding: utf-8 -*-
"""
Created on Mon Jun  1 20:52:26 2020

@author: prati
"""
import numpy as np
import random
import matplotlib.pyplot as plt
import matplotlib.colors
import math
import time



pixels = np.zeros((99,99,3))

K1 = 0.001
K2 = 0.001
K3 = 0.001
K4 = 0.001
t = 0

adjacency_matrix = [[1,1,1],
                    [1,0,1],
                    [1,1,1]]




def neighbors_list(i,j,k):
    temp = []
    for a in range(0,3):
        for b in range(0,3):
            if(adjacency_matrix[a][b] == 1):
                x_diff = a - 1
                y_diff = b - 1
                x_neighbor = i + x_diff
                y_neighbor = j + y_diff
                if(x_neighbor >= 0 and x_neighbor < 98 and y_neighbor >=0 and y_neighbor < 98):
                    arr = []
                    arr.append(x_neighbor)
                    arr.append(y_neighbor)
                    arr.append(k)
                    temp.append(arr)
    return temp


def func(x):
    value = (math.sin((float)(20 * x)))
    return value


def applyCA(i,j,k,t):
    initial_state = pixels[i][j][k]

    K1 = 1
    if(j <= 2):
        K1 = ((240 + 115 + 55)/240)
    elif(j >=3 or j <= 5):
        K1 = ((240 + 115 + 55)/115)
    else:
        K1 = ((240 + 115 + 55)/55)
    
    new_state = initial_state * K1 * Pressure(t)
    neighbors_state = 0
    for n in neighbors_list(i,j,k):
        K3 = 1
        if(n[1] <= 2):
            K3 = ((240 + 115 + 55)/240)
        elif(n[1] >=3 or n[1] <= 5):
            K3 = ((240 + 115 + 55)/115)
        else:
            K3 = ((240 + 115 + 55)/55)
        
        neighbors_state += K3 * func(pixels[n[0]][n[1]][n[2]]) * Pressure(t)
    
    
    
    neighbors_state += func(initial_state)
    
    K2 = 1
    if(j <= 2):
        K2 = ((240 + 115 + 55)/240)
    elif(j >=3 or j <= 5):
        K2 = ((240 + 115 + 55)/115)
    else:
        K2 = ((240 + 115 + 55)/55)
    
    new_state += neighbors_state * K2 * Pressure(t)
    
    K4 = 1
    if(j <= 2):
        K4 = ((240 + 115 + 55)/240)
    elif(j >=3 or j <= 5):
        K4 = ((240 + 115 + 55)/115)
    else:
        K4 = ((240 + 115 + 55)/55)
    
    new_state = new_state +  K4 * random.random()
    if(new_state > 255):
        new_state = 255
    if(new_state < initial_state):
        new_state = initial_state
    return new_state
            
    
def Pressure(t):
    return(math.sin(t))

    
def simulate(t):
    old_arr = np.zeros((99,99,3))
    for i in range(99):
        for j in range(99):
            for k in range(3):
                old_arr[i][j][k] = pixels[i][j][k]
    

    for i in range(99):
        for j in range(99):
            for k in range(3):
                old_arr[i][j][k] = applyCA(i,j,k,t)
                
    for i in range(99):
        for j in range(99):
            for k in range(3):
                pixels[i][j][k] = old_arr[i][j][k]
    return pixels
                
def red(v):
    c = (int)(v)
    if(c < 96):
        return 0
    elif(c < 160):
        return ((float)(c) - 95)/64
    elif(c < 224):
        return 1
    return((288 -(float)(c))/64)


def green(v):
    c = (int)(v)
    if(c < 32):
        return 0
    elif(c < 96):
        return ((float)(c) - 31)/64
    elif(c < 160):
        return 1
    elif(c < 224):
        return((224 - (float)(c))/64)
    return 0
        
def blue(v):
    c = (int)(v)
    if(c < 32):
        return (((float)(c)) + 33) / 64
    elif(c < 96):
        return 1
    elif(c < 160):
        return(160 - ((float)(c))) / 64
    return 0
        
        


while(True):
    simulate(t)
    layer1 = np.zeros((99,99))
    layer2 = np.zeros((99,99))
    layer3 = np.zeros((99,99))
    for i in range(99):
        for j in range(99):
            for k in range(3):
                if(k == 0):
                    layer1[i][j] = pixels[i][j][k]
                elif(k == 1):
                    layer2[i][j] = pixels[i][j][k]
                elif(k == 2):
                    layer3[i][j] = pixels[i][j][k]
                    
    first_layer = np.empty(shape=(99,99,3),dtype='float')
    for i in range(99):
        for j in range(99):
            first_layer[i][j][0] = red(layer1[i][j])
            first_layer[i][j][1] = green(layer1[i][j])
            first_layer[i][j][2] = blue(layer1[i][j])
    second_layer = np.empty(shape=(99,99,3),dtype='float')
    for i in range(99):
        for j in range(99):
            second_layer[i][j][0] = red(layer2[i][j])
            second_layer[i][j][1] = green(layer2[i][j])
            second_layer[i][j][2] = blue(layer2[i][j])
    third_layer = np.empty(shape=(99,99,3),dtype='float')
    for i in range(99):
        for j in range(99):
            third_layer[i][j][0] = red(layer3[i][j])
            third_layer[i][j][1] = green(layer3[i][j])
            third_layer[i][j][2] = blue(layer3[i][j])
            
    first_layer = np.reshape(first_layer, (99,99,3))
    second_layer = np.reshape(second_layer, (99,99,3))
    third_layer = np.reshape(third_layer, (99,99,3))
    print("--------------------------------------------")
    plt.imshow(first_layer)
    plt.show()
    #plt.imshow(second_layer)
    #plt.show()
    #plt.imshow(third_layer)
    #plt.show()
    print("---------------------------------------------")
    t = t + 1
    time.sleep(2)