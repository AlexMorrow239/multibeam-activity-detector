#include "include/NIDAQmx.h"
#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <array>
#include <chrono>
#include <atomic>

// Constants
constexpr int NUM_TUBES = 16;
constexpr int PORT0_LINE_COUNT = 5;
constexpr int PORT1_LINE_COUNT = 2;

// Shared state structure
struct SharedState {
    TaskHandle inputTask = 0;
    TaskHandle outputTask = 0;
    float timebase = 0.0002f;  // Default 0.2ms
    std::atomic<bool> running{true};
    std::mutex mutex;
    std::condition_variable resetCond;    // Signals when reset pulse occurs
    std::condition_variable clockCond;    // Signals clock transitions
    bool resetActive = false;             // Indicates reset pulse is active
    bool clockHigh = false;              // Indicates clock state
    int currentTube = 0;                 // Current tube being read
};

// Tube reading structure
struct TubeReading {
    int value = 0;
    bool isEating = false;
    std::mutex mutex;
};

// Global variables
std::array<TubeReading, NUM_TUBES> tubeReadings;
SharedState state;

// Error checking macro
#define DAQmxErrChk(functionCall) if( DAQmxFailed(error=(functionCall)) ) goto Error; else

// Initialize the DAQ device
int initializeDevice() {
    int error = 0;
    
    // Configure digital input (P0.0-P0.4)
    DAQmxErrChk(DAQmxCreateTask("InputTask", &state.inputTask));
    DAQmxErrChk(DAQmxCreateDIChan(state.inputTask, "Dev1/port0/line0:4", "",
                                 DAQmx_Val_ChanForAllLines));
    
    // Configure digital output (P1.0-P1.1)
    DAQmxErrChk(DAQmxCreateTask("OutputTask", &state.outputTask));
    DAQmxErrChk(DAQmxCreateDOChan(state.outputTask, "Dev1/port1/line0:1", "",
                                 DAQmx_Val_ChanForAllLines));
    
    return 0;

Error:
    return error;
}

// Process data read from a tube
void processData(const std::array<unsigned char, PORT0_LINE_COUNT>& data, int tubeNumber) {
    std::lock_guard<std::mutex> lock(tubeReadings[tubeNumber].mutex);
    
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

// Output thread function - handles reset and clock signals
void outputThreadFunc() {
    std::array<unsigned char, PORT1_LINE_COUNT> outputData{};
    
    while (state.running) {
        // Send reset pulse (P1.0 HIGH for 3Tb)
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            outputData[0] = 1;  // Reset high
            outputData[1] = 0;  // Clock low
            state.resetActive = true;
            state.currentTube = 0;
        }
        state.resetCond.notify_all();
        
        DAQmxWriteDigitalLines(state.outputTask, 1, 1, state.timebase*3.0,
                              DAQmx_Val_GroupByChannel, outputData.data(), nullptr, nullptr);
        
        // Reset low
        outputData[0] = 0;
        DAQmxWriteDigitalLines(state.outputTask, 1, 1, state.timebase,
                              DAQmx_Val_GroupByChannel, outputData.data(), nullptr, nullptr);
        
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.resetActive = false;
        }
        
        // Clock cycles for all tubes
        for (int i = 0; i < NUM_TUBES; i++) {
            // Clock high
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                outputData[1] = 1;
                state.clockHigh = true;
            }
            state.clockCond.notify_all();
            
            DAQmxWriteDigitalLines(state.outputTask, 1, 1, state.timebase*2.5,
                                  DAQmx_Val_GroupByChannel, outputData.data(), nullptr, nullptr);
            
            // Clock low
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                outputData[1] = 0;
                state.clockHigh = false;
            }
            state.clockCond.notify_all();
            
            DAQmxWriteDigitalLines(state.outputTask, 1, 1, state.timebase*2.5,
                                  DAQmx_Val_GroupByChannel, outputData.data(), nullptr, nullptr);
        }
    }
}

// Input thread function - reads data from tubes
void inputThreadFunc() {
    std::array<unsigned char, PORT0_LINE_COUNT> inputData{};
    
    while (state.running) {
        // Wait for reset pulse
        {
            std::unique_lock<std::mutex> lock(state.mutex);
            state.resetCond.wait(lock, []{return state.resetActive;});
        }
        
        // Process all tubes
        for (int i = 0; i < NUM_TUBES; i++) {
            // Wait for clock to go high
            {
                std::unique_lock<std::mutex> lock(state.mutex);
                state.clockCond.wait(lock, []{return state.clockHigh;});
            }
            
            // Wait 1Tb then read data
            std::this_thread::sleep_for(std::chrono::microseconds(
                static_cast<int>(state.timebase * 1000000)));
                
            DAQmxReadDigitalLines(state.inputTask, 1, state.timebase*2.0,
                                DAQmx_Val_GroupByChannel, inputData.data(),
                                PORT0_LINE_COUNT, nullptr, nullptr, nullptr);
            
            // Process the data
            processData(inputData, i);
            
            // Wait for clock to go low
            {
                std::unique_lock<std::mutex> lock(state.mutex);
                state.clockCond.wait(lock, []{return !state.clockHigh;});
            }
        }
    }
}

// Display thread function - updates the console output
void displayThreadFunc() {
    while (state.running) {
        std::cout << "\033[2J\033[H";  // Clear screen and move cursor to top
        std::cout << "Multibeam Activity Detector - Real-time Monitoring\n";
        std::cout << "===============================================\n\n";
        std::cout << "Tube | Position | Status | Activity\n";
        std::cout << "-----|----------|---------|----------\n";
        
        for (int i = 0; i < NUM_TUBES; i++) {
            std::lock_guard<std::mutex> lock(tubeReadings[i].mutex);
            
            std::cout << std::setw(4) << i + 1 << " | ";
            
            if (tubeReadings[i].isEating) {
                std::cout << std::setw(8) << 1 << " | EATING  | Feeding at position 1\n";
            } else if (tubeReadings[i].value > 0) {
                std::cout << std::setw(8) << tubeReadings[i].value 
                         << " | ACTIVE  | Moving at position " 
                         << tubeReadings[i].value << "\n";
            } else {
                std::cout << std::setw(8) << "-" << " | IDLE    | No activity detected\n";
            }
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

int main() {
    int error = initializeDevice();
    if (error) {
        std::cerr << "Failed to initialize device. Error: " << error << "\n";
        return error;
    }
    
    // Configure timebase
    std::cout << "Select timebase (milliseconds):\n";
    std::cout << "1. 0.01\n2. 0.1\n3. 1.0\n4. 10.0\n";
    std::cout << "Choice: ";
    char choice;
    std::cin >> choice;
    
    switch(choice) {
        case '1': state.timebase = 0.00001f; break;
        case '2': state.timebase = 0.0001f; break;
        case '3': state.timebase = 0.001f; break;
        case '4': state.timebase = 0.01f; break;
        default:  std::cout << "Using default timebase (0.2ms)\n";
    }
    
    // Create threads
    std::thread outputThread(outputThreadFunc);
    std::thread inputThread(inputThreadFunc);
    std::thread displayThread(displayThreadFunc);
    
    std::cout << "\nPress Enter to stop acquisition...\n";
    std::cin.ignore();
    std::cin.get();
    
    // Stop acquisition and join threads
    state.running = false;
    outputThread.join();
    inputThread.join();
    displayThread.join();
    
    // Cleanup
    if (state.inputTask != 0) {
        DAQmxStopTask(state.inputTask);
        DAQmxClearTask(state.inputTask);
    }
    if (state.outputTask != 0) {
        DAQmxStopTask(state.outputTask);
        DAQmxClearTask(state.outputTask);
    }
    
    return 0;
}