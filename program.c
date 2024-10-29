#include "include/NIDAQmx.h" // NI DAQ driver library
#include <stdio.h> // Standard input/output library
#include <stdbool.h> // Standard boolean library 
#include <windows.h> // Windows API library, used for Sleep function

// Error checking macro
#define DAQmxErrChk(functionCall) if( DAQmxFailed(error=(functionCall)) ) goto Error; else

// Constants
#define NUM_TUBES 16        // Number of tubes to monitor
#define PORT0_LINE_COUNT 5  // P0.0 to P0.4 for data input
#define PORT1_LINE_COUNT 2  // P1.0 (reset) and P1.1 (clock)

// Global variables
TaskHandle inputTask = 0;    // Handle for input task, keeps track of the task, and allows for communication with the task
TaskHandle outputTask = 0;   // Handle for output task, keeps track of the task, and allows for communication with the task
float timebase = 0.0002f;    // Default 0.2ms (for 1KHz clock)

// Function prototypes
int initializeDevice(void);
int configureTimebase(void);
void cleanup(void);
int runAcquisition(void);
void processData(unsigned char data[], int tubeNumber);
void displayTable(void);

// Table to store tube readings
typedef struct {
    int value; // Value of the tube, position of the fly in the tube
    bool isEating; // Indicates if the fly is eating
} TubeReading;

TubeReading tubeReadings[NUM_TUBES]; // Array to store tube readings for all tubes

int main(void) {
    int error = 0; // Error code
    char inputBuffer[10]; // Buffer to store user input
    
    printf("Multibeam Activity Detector Control Program\n");
    printf("=========================================\n\n");
    
    // Initialize the device
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
        case '1': timebase = 0.00001f; break;
        case '2': timebase = 0.0001f; break;
        case '3': timebase = 0.001f; break;
        case '4': timebase = 0.01f; break;
        default:  printf("Using default timebase (0.2ms)\n");
    }
    
    // Main acquisition loop
    printf("\nStarting acquisition. Press Ctrl+C to stop.\n\n");
    while(1) {
        error = runAcquisition();
        if (error) {
            printf("Acquisition error: %d\n", error);
            break;
        }
        displayTable();
        Sleep(100);  // Small delay between iterations
    }
    
    cleanup();
    return error;
}

int initializeDevice(void) {
    int error = 0; // Error code to track errors
    
    // Configure digital input (P0.0-P0.4)
    DAQmxErrChk(DAQmxCreateTask("InputTask", &inputTask));
    DAQmxErrChk(DAQmxCreateDIChan(inputTask, "Dev1/port0/line0:4", "",
                                 DAQmx_Val_ChanForAllLines));
    
    // Configure digital output (P1.0-P1.1)
    DAQmxErrChk(DAQmxCreateTask("OutputTask", &outputTask));
    DAQmxErrChk(DAQmxCreateDOChan(outputTask, "Dev1/port1/line0:1", "",
                                 DAQmx_Val_ChanForAllLines));
    
    return 0;

Error:
    cleanup();
    return error;
}

int runAcquisition(void) {
    int error = 0; // Error code to track errors
    unsigned char inputData[PORT0_LINE_COUNT]; // Buffer to store input data
    unsigned char outputData[PORT1_LINE_COUNT]; // Buffer to store output data
    int tubeCounter; // Counter for the number of tubes
    
    // Step 1: Send reset pulse (P1.0 HIGH for 3Tb)
    outputData[0] = 1;  // Reset high
    outputData[1] = 0;  // Clock low
    DAQmxErrChk(DAQmxWriteDigitalLines(outputTask, 1, 1, timebase*3.0,
                                      DAQmx_Val_GroupByChannel, outputData, NULL, NULL));
    
    outputData[0] = 0;
    DAQmxErrChk(DAQmxWriteDigitalLines(outputTask, 1, 1, timebase,
                                      DAQmx_Val_GroupByChannel, outputData, NULL, NULL));
    
    // Main acquisition loop for all tubes
    for(tubeCounter = 0; tubeCounter < NUM_TUBES; tubeCounter++) {
        // Step 2: Send clock pulse (P1.1 HIGH)
        outputData[1] = 1;
        DAQmxErrChk(DAQmxWriteDigitalLines(outputTask, 1, 1, timebase*2.5,
                                          DAQmx_Val_GroupByChannel, outputData, NULL, NULL));
        
        // Step 4-5: Wait 1Tb and read data during 2Tb interval
        Sleep((DWORD)(timebase * 1000));
        DAQmxErrChk(DAQmxReadDigitalLines(inputTask, 1, timebase*2.0,
                                         DAQmx_Val_GroupByChannel, inputData,
                                         PORT0_LINE_COUNT, NULL, NULL, NULL));
        
        // Process the read data
        processData(inputData, tubeCounter);
        
        // Step 7: Wait 2Tb
        Sleep((DWORD)(timebase * 2000));
        
        // Clock low
        outputData[1] = 0;
        DAQmxErrChk(DAQmxWriteDigitalLines(outputTask, 1, 1, timebase*2.5,
                                          DAQmx_Val_GroupByChannel, outputData, NULL, NULL));
    }
    
    return 0;

Error:
    return error;
}

void processData(unsigned char data[], int tubeNumber) {
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
}

void displayTable(void) {
    int i;
    printf("\033[2J\033[H");  // Clear screen and move cursor to top
    printf("Multibeam Activity Detector - Real-time Monitoring\n");
    printf("===============================================\n\n");
    printf("Tube | Position | Status | Activity\n");
    printf("-----|----------|---------|----------\n");
    
    for(i = 0; i < NUM_TUBES; i++) {
        printf("%4d | ", i + 1);  // Tube number
        
        if (tubeReadings[i].isEating) {
            printf("%8d | EATING  | Feeding at position 1\n", 1);
        } else if (tubeReadings[i].value > 0) {
            printf("%8d | ACTIVE  | Moving at position %d\n", 
                   tubeReadings[i].value, 
                   tubeReadings[i].value);
        } else {
            printf("%8s | IDLE    | No activity detected\n", "-");
        }
    }
    printf("\n");
    printf("Legend:\n");
    printf("- EATING: Fly is feeding at position 1\n");
    printf("- ACTIVE: Fly is moving, position indicates beam location\n");
    printf("- IDLE: No fly detected at this tube\n\n");
}

void cleanup(void) {
    if (inputTask != 0) {
        DAQmxStopTask(inputTask);
        DAQmxClearTask(inputTask);
    }
    if (outputTask != 0) {
        DAQmxStopTask(outputTask);
        DAQmxClearTask(outputTask);
    }
}