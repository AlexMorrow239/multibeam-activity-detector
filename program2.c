#include "include/NIDAQmx.h"
#include <stdio.h>
#include <stdbool.h>
#include <windows.h>
#include <pthread.h>

#define NUM_TUBES 16
#define PORT0_LINE_COUNT 5
#define PORT1_LINE_COUNT 2
#define DAQmxErrChk(functionCall) if( DAQmxFailed(error=(functionCall)) ) goto Error; else

// Shared state between threads
typedef struct {
    TaskHandle inputTask;
    TaskHandle outputTask;
    float timebase;
    volatile bool running;
    volatile int currentTube;
    volatile bool resetActive;
    volatile bool clockHigh;
    pthread_mutex_t mutex;
    pthread_cond_t resetCond;
    pthread_cond_t clockCond;
} DAQState;

// Structure for tube readings
typedef struct {
    int value;
    bool isEating;
    pthread_mutex_t mutex;
} TubeReading;

// Global variables
TubeReading tubeReadings[NUM_TUBES];
DAQState daqState = {
    .inputTask = 0,
    .outputTask = 0,
    .timebase = 0.0002f,
    .running = false,
    .currentTube = 0,
    .resetActive = false,
    .clockHigh = false
};

// Function prototypes
void* outputThreadFunc(void* arg);
void* inputThreadFunc(void* arg);
int initializeDevice(void);
void cleanup(void);
void processData(unsigned char data[], int tubeNumber);
void displayTable(void);

int main(void) {
    int error = 0;
    char inputBuffer[10];
    pthread_t outputThread, inputThread;
    
    // Initialize mutexes and condition variables
    pthread_mutex_init(&daqState.mutex, NULL);
    pthread_cond_init(&daqState.resetCond, NULL);
    pthread_cond_init(&daqState.clockCond, NULL);
    
    // Initialize tube reading mutexes
    for(int i = 0; i < NUM_TUBES; i++) {
        pthread_mutex_init(&tubeReadings[i].mutex, NULL);
    }
    
    printf("Multibeam Activity Detector Control Program\n");
    printf("=========================================\n\n");
    
    // Initialize device
    error = initializeDevice();
    if (error) {
        printf("Failed to initialize device. Error: %d\n", error);
        return error;
    }
    
    // Configure timebase
    printf("Select timebase (milliseconds):\n");
    printf("1. 0.01\n2. 0.1\n3. 1.0\n4. 10.0\n");
    printf("Choice: ");
    fgets(inputBuffer, sizeof(inputBuffer), stdin);
    switch(inputBuffer[0]) {
        case '1': daqState.timebase = 0.00001f; break;
        case '2': daqState.timebase = 0.0001f; break;
        case '3': daqState.timebase = 0.001f; break;
        case '4': daqState.timebase = 0.01f; break;
        default:  printf("Using default timebase (0.2ms)\n");
    }
    
    // Start acquisition
    printf("\nStarting acquisition. Press Enter to stop.\n\n");
    daqState.running = true;
    
    // Create threads
    pthread_create(&outputThread, NULL, outputThreadFunc, &daqState);
    pthread_create(&inputThread, NULL, inputThreadFunc, &daqState);
    
    // Wait for user input to stop
    getchar();
    
    // Stop acquisition
    pthread_mutex_lock(&daqState.mutex);
    daqState.running = false;
    pthread_mutex_unlock(&daqState.mutex);
    
    // Wait for threads to finish
    pthread_join(outputThread, NULL);
    pthread_join(inputThread, NULL);
    
    // Cleanup
    cleanup();
    
    // Destroy mutexes and condition variables
    pthread_mutex_destroy(&daqState.mutex);
    pthread_cond_destroy(&daqState.resetCond);
    pthread_cond_destroy(&daqState.clockCond);
    
    for(int i = 0; i < NUM_TUBES; i++) {
        pthread_mutex_destroy(&tubeReadings[i].mutex);
    }
    
    return 0;
}

void* outputThreadFunc(void* arg) {
    DAQState* state = (DAQState*)arg;
    int error = 0;
    unsigned char outputData[PORT1_LINE_COUNT] = {0, 0};
    
    while(1) {
        pthread_mutex_lock(&state->mutex);
        if (!state->running) {
            pthread_mutex_unlock(&state->mutex);
            break;
        }
        pthread_mutex_unlock(&state->mutex);
        
        // Send reset pulse (P1.0 HIGH for 3Tb)
        outputData[0] = 1;  // Reset high
        outputData[1] = 0;  // Clock low
        
        pthread_mutex_lock(&state->mutex);
        state->resetActive = true;
        state->currentTube = 0;
        pthread_cond_broadcast(&state->resetCond);
        pthread_mutex_unlock(&state->mutex);
        
        DAQmxWriteDigitalLines(state->outputTask, 1, 1, state->timebase*3.0,
                              DAQmx_Val_GroupByChannel, outputData, NULL, NULL);
        
        outputData[0] = 0;  // Reset low
        DAQmxWriteDigitalLines(state->outputTask, 1, 1, state->timebase,
                              DAQmx_Val_GroupByChannel, outputData, NULL, NULL);
        
        pthread_mutex_lock(&state->mutex);
        state->resetActive = false;
        pthread_mutex_unlock(&state->mutex);
        
        // Clock cycles for all tubes
        for(int i = 0; i < NUM_TUBES; i++) {
            // Clock high
            outputData[1] = 1;
            
            pthread_mutex_lock(&state->mutex);
            state->clockHigh = true;
            pthread_cond_broadcast(&state->clockCond);
            pthread_mutex_unlock(&state->mutex);
            
            DAQmxWriteDigitalLines(state->outputTask, 1, 1, state->timebase*2.5,
                                  DAQmx_Val_GroupByChannel, outputData, NULL, NULL);
            
            // Clock low
            outputData[1] = 0;
            
            pthread_mutex_lock(&state->mutex);
            state->clockHigh = false;
            state->currentTube = (state->currentTube + 1) % NUM_TUBES;
            pthread_mutex_unlock(&state->mutex);
            
            DAQmxWriteDigitalLines(state->outputTask, 1, 1, state->timebase*2.5,
                                  DAQmx_Val_GroupByChannel, outputData, NULL, NULL);
        }
    }
    
    return NULL;
}

void* inputThreadFunc(void* arg) {
    DAQState* state = (DAQState*)arg;
    int error = 0;
    unsigned char inputData[PORT0_LINE_COUNT];
    
    while(1) {
        pthread_mutex_lock(&state->mutex);
        if (!state->running) {
            pthread_mutex_unlock(&state->mutex);
            break;
        }
        
        // Wait for reset pulse
        while(!state->resetActive && state->running) {
            pthread_cond_wait(&state->resetCond, &state->mutex);
        }
        pthread_mutex_unlock(&state->mutex);
        
        // Main acquisition loop
        for(int i = 0; i < NUM_TUBES && state->running; i++) {
            pthread_mutex_lock(&state->mutex);
            // Wait for clock to go high
            while(!state->clockHigh && state->running) {
                pthread_cond_wait(&state->clockCond, &state->mutex);
            }
            pthread_mutex_unlock(&state->mutex);
            
            // Wait 1Tb
            Sleep((DWORD)(state->timebase * 1000));
            
            // Read data during 2Tb interval
            DAQmxReadDigitalLines(state->inputTask, 1, state->timebase*2.0,
                                DAQmx_Val_GroupByChannel, inputData,
                                PORT0_LINE_COUNT, NULL, NULL, NULL);
            
            // Process the read data
            processData(inputData, i);
            
            // Wait 2Tb
            Sleep((DWORD)(state->timebase * 2000));
        }
        
        // Update display after complete cycle
        displayTable();
    }
    
    return NULL;
}

void processData(unsigned char data[], int tubeNumber) {
    pthread_mutex_lock(&tubeReadings[tubeNumber].mutex);
    
    if (data[4] == 0) {  // DV is LOW - normal position reading
        tubeReadings[tubeNumber].value = 
            data[0] + (data[1] << 1) + (data[2] << 2) + (data[3] << 3);
        tubeReadings[tubeNumber].isEating = false;
    }
    else {  // DV is HIGH - check for eating condition
        if (data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 0 &&
            tubeReadings[tubeNumber].value == 1) {
            tubeReadings[tubeNumber].isEating = true;
        }
    }
    
    pthread_mutex_unlock(&tubeReadings[tubeNumber].mutex);
}

// [Previous initializeDevice(), cleanup(), and displayTable() functions remain the same]