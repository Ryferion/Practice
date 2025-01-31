#include <iostream>
#include <fstream>

#include <bitset>
#include <chrono>
#include <pthread.h>

#include "hip/hip_runtime.h"
#include "rocm_smi/rocm_smi.h"

#define __HIP_PLATFORM_HCC__
#define DEVICE_NUM 2
#define TILE_SIZE 16

using namespace std;
using namespace std::chrono;

// can get a corresponding error string by
#define HIP_CHECK(command) {        \
    hipError_t status = command;    \
    if (status != hipSuccess) {     \
        std::cerr << "Error: HIP reports " << hipGetErrorString(status) << std::endl;   \
        std::abort(); } }


#define PRINT_RSMI_ERR(RET) { \
  if (RET != RSMI_STATUS_SUCCESS) { \
    const char *err_str; \
    std::cout << "RSMI call returned " << (RET) \
      << " at line " << __LINE__ << std::endl; \
      rsmi_status_string((RET), &err_str); \
      std::cout << err_str << std::endl; \
  } \
}

#define CHK_RSMI_RET(RET) { \
  PRINT_RSMI_ERR(RET) \
  if (RET != RSMI_STATUS_SUCCESS) { \
    return (RET); \
  } \
}

#define CHK_RSMI_RET_I(RET) { \
  PRINT_RSMI_ERR(RET) \
  if (RET != RSMI_STATUS_SUCCESS) { \
    return static_cast<int>(RET); \
  } \
}

#define CHK_RSMI_PERM_RET(RET) { \
    if ((RET) == RSMI_STATUS_PERMISSION) { \
      std::cout << "This command requires root access." << std::endl; \
    } else { \
      CHK_RSMI_RET_I(RET) \
    } \
}

__global__ void matrixMultiply(int row, int col, int out, const float *A, const float *B, float *C)
{
    /*
    A = row x col
    B = col x out
    C = row x out
    */
    
    // blocking to be more cache coherent
    __shared__ float sharedM1[TILE_SIZE][TILE_SIZE];
    __shared__ float sharedM2[TILE_SIZE][TILE_SIZE];
    
    int xThread = threadIdx.x;
    int yThread = threadIdx.y;

    int xIdx = xThread + blockIdx.x * blockDim.x; // current col
    int yIdx = yThread + blockIdx.y * blockDim.y; // current row
    
    float temp = 0;
    
    int i = 0;
    for (i = 0; i < (TILE_SIZE + out - 1) / TILE_SIZE; i++)
    {
        int xPos = i * TILE_SIZE + xThread;
        if ((yIdx < row) && (xPos < out))
        {
            sharedM1[yThread][xThread] = A[yIdx * out + xPos];
        }
        else
        {
            sharedM1[yThread][xThread] = 0.0;
        }

        int yPos = i * TILE_SIZE + yThread;
        if ((xIdx < col) && (yPos < out))
        {
            sharedM2[yThread][xThread] = B[xIdx * col + yPos];
        }
        else
        {
            sharedM2[yThread][xThread] = 0.0;
        }

        __syncthreads();

        // combine blocks
        for (int j = 0; j < TILE_SIZE; j++)
        {
            temp += sharedM1[yThread][j] * sharedM2[j][xThread];
        }

        __syncthreads();

        if ((yIdx < row) && (xIdx < col))
        {
            C[yIdx * col + xIdx] = temp;
        }
    }
}

void matrixWrite(int rowSize, int colSize, float *input, string fileName)
{
    fstream outFile;
    outFile.open(fileName, std::fstream::out | std::fstream::trunc);

    for (int i = 0; i < rowSize; i++)
    {   
        for (int j = 0; j < colSize; j++)
        {
            outFile << input[j + j * i];
            if ((j + 1) != colSize)
            {
                outFile << " ";
            }
        }
        outFile << "\n";
    }
    outFile.close();
}


void matrixRead(string fileName, float *readTo, int size)
{
    // =================================== read in matrix ===================================
    int counter = 0;
    ifstream outFile (fileName);
    if (outFile.is_open())
    {
        string temp;
        while (outFile >> temp)
        {
            readTo[counter] = stof(temp);
            counter++;
        }
    }
}

void* powerCheck(void *args)
{
    
}

struct arguments {
    int arg_mask;
    int arg_row;
    int arg_col;
    int arg_out;
    string arg_firstMatrix;
    string arg_secondMatrix;
    string arg_thirdMatrix;
};

void* hip(void *args)
{
    struct arguments *inputArgs = (struct arguments*) args;
    int mask = inputArgs->arg_mask;
    int row = inputArgs->arg_row;
    int col = inputArgs->arg_row;
    int out = inputArgs->arg_out;
    string matrixOne = inputArgs->arg_firstMatrix;
    string matrixTwo = inputArgs->arg_secondMatrix;
    string matrixThree = inputArgs->arg_thirdMatrix;
    
    int iter = 0;

    // memory related variables
    uint32_t CUMask[2];
    const uint32_t CUMask_size = 2;

    CUMask[0] = 0x00000001;
    CUMask[1] = 0x00000001; 

    float *A_host, *B_host, *C_host;
    float *A_device, *B_device, *C_device;
    size_t A_size, B_size, C_size;

    // set mask stuff
    if (iter != 0)
    {
        // CUMask = CUMask * 2 + 1;  
    }

    if (mask == 44)
    {
        CUMask[0] = 0x3fffffff;
        CUMask[1] = 0x00000000;
    }
    if (mask == 444)
    {
        CUMask[0] = 0x00000000;
        CUMask[1] = 0x3fffffff;
    }

    // print mask to check
    // cout << " CUMask: " << std::bitset<32>(CUMask) << endl;
    
    // create streams
    hipStream_t streamMultiply;
    hipStream_t streamMemory;

    A_size = row * col;
    B_size = col * out;
    C_size = row * out;
    
    // allocate host memory
    HIP_CHECK(hipHostMalloc((void**) &A_host, sizeof(float) * A_size));
    HIP_CHECK(hipHostMalloc((void**) &B_host, sizeof(float) * B_size));
    HIP_CHECK(hipHostMalloc((void**) &C_host, sizeof(float) * C_size));
    
    // fill host matrices with stuff from text files
    matrixRead(matrixOne, A_host, A_size);
    matrixRead(matrixTwo, B_host, B_size);

    // matrix multiplication

    // allocate memory for device
    HIP_CHECK(hipMalloc((void**) &A_device, sizeof(float) * A_size));
    HIP_CHECK(hipMalloc((void**) &B_device, sizeof(float) * B_size));
    HIP_CHECK(hipMalloc((void**) &C_device, sizeof(float) * C_size));

    // copy data from host to device using stream...
    HIP_CHECK(hipExtStreamCreateWithCUMask(&streamMultiply, CUMask_size, CUMask)); 
    HIP_CHECK(hipExtStreamCreateWithCUMask(&streamMemory, CUMask_size, CUMask)); 

    HIP_CHECK(hipMemcpyAsync(A_device, A_host, sizeof(float) * A_size, hipMemcpyHostToDevice, streamMemory));
    HIP_CHECK(hipMemcpyAsync(B_device, B_host, sizeof(float) * B_size, hipMemcpyHostToDevice, streamMemory));

    // set up block dim and thread dim
    dim3 blocks(col / TILE_SIZE + 1, row / TILE_SIZE + 1, 1); // 3D dimensions of the grid of blocks
    dim3 threads(TILE_SIZE, TILE_SIZE, 1); // 3D dimensions of a block of threads

    // start timer: gear it towards kernel stuff
    auto start = high_resolution_clock::now();

    // launch kernel
    hipLaunchKernelGGL(matrixMultiply, blocks, threads, 0, streamMultiply, row, col, out, A_device, B_device, C_device);

    HIP_CHECK(hipGetLastError());

    HIP_CHECK(hipStreamSynchronize(streamMemory));
    
    // copy matrix data from device to host
    HIP_CHECK(hipMemcpyAsync(C_host, C_device, sizeof(float) * C_size, hipMemcpyDeviceToHost, streamMemory)); // host waits for kernel to finish here since hipMemcpy is blocking

    HIP_CHECK(hipStreamDestroy(streamMultiply));
    HIP_CHECK(hipStreamDestroy(streamMemory));

    HIP_CHECK(hipFree(A_device)); // free device memory
    HIP_CHECK(hipFree(B_device)); // free device memory
    HIP_CHECK(hipFree(C_device)); // free device memory
    
    // write to .txt
    // matrixWrite(row, out, C_host, matrixThree);

    HIP_CHECK(hipHostFree(A_host)); // free pinned memory
    HIP_CHECK(hipHostFree(B_host)); // free pinned memory
    HIP_CHECK(hipHostFree(C_host)); // free pinned memory

    // end timer
    auto stop = high_resolution_clock::now();
    auto duration = duration_cast<microseconds>(stop - start);
    cout << "Time taken by function: " << duration.count() << " microseconds, with CU mask " << std::bitset<30>(CUMask[0]) << std::bitset<30>(CUMask[1]) <<  endl;

    return NULL;
}

int main(int argc, char **argv)
{
    cout << "C++ version: ";
    if (__cplusplus == 201703L) std::cout << "C++17\n";
    else if (__cplusplus == 201402L) std::cout << "C++14\n";
    else if (__cplusplus == 201103L) std::cout << "C++11\n";
    else if (__cplusplus == 199711L) std::cout << "C++98\n";
    else std::cout << "pre-standard C++\n";

    int deviceCount = -1, deviceID = -1, CUCount = -1;

    HIP_CHECK(hipSetDevice(DEVICE_NUM)); // use GPU 2
    HIP_CHECK(hipGetDevice(&deviceID)); 
    HIP_CHECK(hipGetDeviceCount(&deviceCount)); // how many devices there be (should be 8 on idk)
    
    hipDeviceProp_t deviceProps;
    HIP_CHECK(hipGetDeviceProperties(&deviceProps, deviceID))

    cout << " Current Device: " << deviceID << endl;
    cout << " CU count: " << deviceProps.multiProcessorCount << endl;
    if (deviceID != 2)
    {
        return 0;
    }

    /*
    A = row x col
    B = col x out
    C = row x out
    */

    int mask = 1;
    int row, col, out;
    string matrixOne, matrixTwo, matrixThree;
        row = 8;
        col = 8;
        out = 8;
        matrixOne = "matrix1.txt";
        matrixTwo = "matrix2.txt";
        matrixThree = "matrix3.txt";

    if (argv[1] != NULL)
    {
        row = atoi(argv[1]);
        col = row;
        out = col;
    }

    if (argv[2] != NULL)
    {
        mask = atoi(argv[2]);
    }

    // thread 1
    pthread_t pthread_id;
    struct arguments *firstHalf = (struct arguments *) malloc(sizeof(struct arguments));
    firstHalf->arg_mask = 44;
    firstHalf->arg_row = row;
    firstHalf->arg_col = col;
    firstHalf->arg_out = out;
    firstHalf->arg_firstMatrix = matrixOne;
    firstHalf->arg_secondMatrix = matrixTwo;
    firstHalf->arg_thirdMatrix = matrixThree;

    struct arguments *secondHalf = (struct arguments *) malloc(sizeof(struct arguments));
    secondHalf->arg_mask = 444;
    secondHalf->arg_row = row;
    secondHalf->arg_col = col;
    secondHalf->arg_out = out;
    secondHalf->arg_firstMatrix = matrixOne;
    secondHalf->arg_secondMatrix = matrixTwo;
    secondHalf->arg_thirdMatrix = matrixThree;

    pthread_create(&pthread_id, NULL, &hip, (void *)firstHalf);
    pthread_create(&pthread_id, NULL, &hip, (void *)secondHalf);
    pthread_join(pthread_id, NULL);

    // pthread_exit(NULL);
    free(firstHalf);
    free(secondHalf);
    return 0;
}
