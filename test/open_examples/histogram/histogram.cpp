/*
 * Copyright (c) 2017, Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "cm_rt.h"

// Includes bitmap_helpers.h for bitmap file open/save operations.
#include "common/bitmap_helpers.h"

// Includes cm_rt_helpers.h to convert the integer return code returned from
// the CM runtime to a meaningful string message.
#include "common/cm_rt_helpers.h"

// Includes isa_helpers.h to load the ISA file generated by the CM compiler.
#include "common/isa_helpers.h"

using namespace std;

#define NUM_BINS 256

// This function prints the histogram of the image on console.
int WriteHist(unsigned *hist) {
    int total = 0;

    // printf("\nHistogram: \n");
    for (int i = 0; i < NUM_BINS; i += 8) {
        // printf("\n  [%03d - %03d]: ", i, i + 7);
        for (int j = 0; j < 8; j++) {
            // printf("%08d  ", hist[i + j]);
            total += hist[i + j];
        }
    }
    printf("\nTotal = %08d \n", total);
    return total;
}

int main(int argc, char *argv[]) {
    // Initializes image size and image name.
    unsigned int width = 256;
    unsigned int height = 256;
    char *image_name = "wave256x256.nv12";
    printf("Processing image %s\n", image_name);

    // Loads an input image named "wave256x256.nv12"  to src_image.
    FILE *fp = fopen(image_name, "rb");
    if (fp == NULL) {
        fprintf(stderr, "Error opening file %s", image_name);
        exit(1);
    }
    unsigned char *src_image = new unsigned char[width * height];
    if (src_image == nullptr) {
        fprintf(stderr, "Out of memory");
        exit(1);
    }
    if (fread(src_image, 1, width * height, fp) != width * height) {
        fprintf(stderr, "Error reading Y from %s\n", image_name);
        exit(1);
    }

    // Uses arbitrarily 60 threads to split up the workload.
    unsigned int num_hist_threads = 60;
    unsigned int *hist = new unsigned[num_hist_threads *
                                      sizeof(unsigned) * 256];
    if (hist == nullptr) {
        fprintf(stderr, "Out of memory");
        exit(1);
    }

    // Here, for "histogram" kernel, each thread works on a block of 8x32 pixels.
    // The thread width is equal to input image width divided by 32.
    // The thread height is equal to input image height divided by 8.
    int thread_width = width / 32;
    int thread_height = height / 8;

    // Creates a CmDevice from scratch.
    // Param device: pointer to the CmDevice object.
    // Param version: CM API version supported by the runtime library.
    CmDevice *device = nullptr;;
    unsigned int version = 0;
    cm_result_check(::CreateCmDevice(device, version));

    // The file histogram_genx.isa is generated when the kernels in the file
    // histogram_genx.cpp are compiled by the CM compiler.
    // Reads in the virtual ISA from "histogram_genx.isa" to the code
    // buffer.
    std::string isa_code = cm::util::isa::loadFile("histogram_genx.isa");
    if (isa_code.size() == 0) {
        std::cerr << "Error: empty ISA binary.\n";
        std::exit(1);
    }

    // Creates a CmProgram object consisting of the kernels loaded from the code
    // buffer.
    // Param isa_code.data(): Pointer to the code buffer containing the virtual
    // ISA.
    // Param isa_code.size(): Size in bytes of the code buffer containing the
    // virtual ISA.
    CmProgram *program = nullptr;
    cm_result_check(device->LoadProgram(const_cast<char *>(isa_code.data()),
                                        isa_code.size(),
                                        program));

    // Creates the kernel.
    // Param program: CM Program from which the kernel is created.
    // Param "histogram": The kernel name which should be no more than 256 bytes
    // including the null terminator.
    CmKernel *kernel0 = nullptr;
    cm_result_check(device->CreateKernel(program,
                                         "histogram",
                                         kernel0));

    // Creates input surface with given width and height in pixels and format.
    CmSurface2D *input_surface = nullptr;
    cm_result_check(device->CreateSurface2D(width,
                                            height,
                                            CM_SURFACE_FORMAT_A8,
                                            input_surface));

    // Copies system memory content to the input surface using the CPU. The
    // system memory content is the data of the input image. The size of data
    // copied is the size of data in the surface.
    cm_result_check(input_surface->WriteSurface(src_image, nullptr));

    // Creates the output surface.
    CmSurface2D *output_surface = nullptr;
    cm_result_check(device->CreateSurface2D(32,
                                            32 * num_hist_threads,
                                            CM_SURFACE_FORMAT_A8,
                                            output_surface));

    // Creates a CmThreadSpace object.
    // There are two usage models for the thread space. One is to define the
    // dependency between threads to run in the GPU. The other is to define a
    // thread space where each thread can get a pair of coordinates during
    // kernel execution. For this example, we use the latter usage model.
    CmThreadSpace *thread_space0 = nullptr;
    cm_result_check(device->CreateThreadSpace(num_hist_threads,
                                              1,
                                              thread_space0));

    // When a surface is created by the CmDevice a SurfaceIndex object is
    // created. This object contains a unique index value that is mapped to the
    // surface.
    // Gets the input surface index.
    SurfaceIndex *input_surface_idx = nullptr;
    cm_result_check(input_surface->GetIndex(input_surface_idx));

    // Gets the output surface index.
    SurfaceIndex *output_surface_idx = nullptr;
    cm_result_check(output_surface->GetIndex(output_surface_idx));

    // Sets a per kernel argument.
    // Sets input surface index as the first argument of kernel.
    // Sets output surface index as the second argument of kernel.
    // Sets the thread number, the thread width, the thread height as the
    // third, the fourth, the fifth argument of kernel.
    cm_result_check(kernel0->SetKernelArg(0,
                                          sizeof(SurfaceIndex),
                                          input_surface_idx));
    cm_result_check(kernel0->SetKernelArg(1,
                                          sizeof(SurfaceIndex),
                                          output_surface_idx));
    cm_result_check(kernel0->SetKernelArg(2,
                                          4,
                                          &num_hist_threads));
    cm_result_check(kernel0->SetKernelArg(3,
                                          4,
                                          &thread_width));
    cm_result_check(kernel0->SetKernelArg(4,
                                          4,
                                          &thread_height));

    // Creates a task queue.
    // The CmQueue is an in-order queue. Tasks get executed according to the
    // order they are enqueued. The next task does not start execution until the
    // current task finishes.
    CmQueue *cmd_queue = nullptr;
    cm_result_check(device->CreateQueue(cmd_queue));

    // Creates a CmTask object.
    // The CmTask object is a container for CmKernel pointers. It is used to
    // enqueue the kernels for execution.
    CmTask *task = nullptr;
    cm_result_check(device->CreateTask(task));

    // Adds a CmKernel pointer to CmTask.
    cm_result_check(task->AddKernel(kernel0));

    // Launches the task on the GPU. Enqueue is a non-blocking call, i.e. the
    // function returns immediately without waiting for the GPU to start or
    // finish execution of the task. The runtime will query the HW status. If
    // the hardware is not busy, the runtime will submit the task to the
    // driver/HW; otherwise, the runtime will submit the task to the driver/HW
    // at another time.
    // An event, "sync_event", is created to track the status of the task.
    CmEvent *sync_event = nullptr;
    cm_result_check(cmd_queue->Enqueue(task,
                                       sync_event,
                                       thread_space0));

    // Destroys a CmTask object.
    // CmTask will be destroyed when CmDevice is destroyed.
    // Here, the application destroys the CmTask object by itself.
    cm_result_check(device->DestroyTask(task));

    // Reads the output surface content to the system memory using the CPU.
    // The size of data copied is the size of data in Surface.
    // It is a blocking call. The function will not return until the copy
    // operation is completed.
    // The dependent event "sync_event" ensures that the reading of the surface
    // will not happen until its state becomes CM_STATUS_FINISHED.
    cm_result_check(output_surface->ReadSurface((unsigned char *)hist,
                                                sync_event));


    // Creates the kernel.
    // Param program: CM Program from which the kernel is created.
    // Param "sum": The kernel name which should be no more than 256 bytes
    // including the null terminator.
    CmKernel *kernel1 = nullptr;
    cm_result_check(device->CreateKernel(program,
                                         "sum",
                                         kernel1));

    // Copies the system memory content to input surface using the CPU.
    // Here we use the output of kernel0 as the input of kernel1.
    cm_result_check(output_surface->WriteSurface((unsigned char *)hist,
                                                 nullptr));

    // Each CmKernel can be executed by multiple concurrent threads.
    // Sets arbitrarily thread count as 12.
    unsigned int num_sum_threads = 12;

    // Creates a CmThreadSpace object.
    // There are two usage models for the thread space. One is to define the
    // dependency between threads to run in the GPU. The other is to define a
    // thread space where each thread can get a pair of coordinates during
    // kernel execution. For this example, we use the latter usage model.
    CmThreadSpace *thread_space1 = nullptr;
    cm_result_check(device->CreateThreadSpace(num_sum_threads,
                                              1,
                                              thread_space1));

    // Sets a per kernel argument.
    // Sets the output surface of kernel0 as the first argument of kernel.
    // Sets the thread count of the kernel "sum" and the thread count of the
    // kernel "histogram" as the second and third argument of kernel.
    cm_result_check(kernel1->SetKernelArg(0,
                                          sizeof(SurfaceIndex),
                                          output_surface_idx));
    int num_histograms = num_hist_threads;
    cm_result_check(kernel1->SetKernelArg(1,
                                          4,
                                          &num_sum_threads));
    cm_result_check(kernel1->SetKernelArg(2,
                                          4,
                                          &num_histograms));

    // Creates a CmTask object.
    // The CmTask object is a container for CmKernel pointers. It is used to
    // enqueue the kernels for execution.
    cm_result_check(device->CreateTask(task));

    // Adds a CmKernel pointer to CmTask.
    cm_result_check(task->AddKernel(kernel1));

    // Launches the task on the GPU. Enqueue is a non-blocking call, i.e. the
    // function returns immediately without waiting for the GPU to start or
    // finish execution of the task. The runtime will query the HW status. If
    // the hardware is not busy, the runtime will submit the task to the
    // driver/HW; otherwise, the runtime will submit the task to the driver/HW
    // at another time.
    cm_result_check(cmd_queue->Enqueue(task,
                                       sync_event,
                                       thread_space1));

    // Destroys a CmTask object.
    // CmTask will be destroyed when CmDevice is destroyed.
    // Here, the application destroys the CmTask object by itself.
    cm_result_check(device->DestroyTask(task));

    // Reads the output surface content to the system memory using the CPU.
    // The size of data copied is the size of data in Surface.
    // It is a blocking call. The function will not return until the copy
    // operation is completed.
    // The dependent event "sync_event" ensures that the reading of the surface
    // will not happen until its state becomes CM_STATUS_FINISHED.
    cm_result_check(output_surface->ReadSurface((unsigned char *)hist,
                                                sync_event));

    // Creates the kernel.
    // Param program: CM Program from which the kernel is created.
    // Param "final_sum": The kernel name which should be no more than 256 bytes
    // including the null terminator.
    CmKernel *kernel2 = nullptr;
    cm_result_check(device->CreateKernel(program,
                                         "final_sum",
                                         kernel2));

    // Copies the system memory content to input surface using the CPU.
    // Here we use the output of kernel1 as the input of kernel2.
    cm_result_check(output_surface->WriteSurface((unsigned char *)hist,
                                                 nullptr));

    // Each CmKernel can be executed by multiple concurrent threads.
    // This function sets the number of threads to spawn on the GPU for this
    // kernel.
    // Only 1 thread uesd here.
    cm_result_check(kernel2->SetThreadCount(1));

    // Sets a per kernel argument.
    // Sets the output of kernel1 as the first argument of kernel.
    // Sets the kernel "sum" thread num as the second argument of kernel.
    cm_result_check(kernel2->SetKernelArg(0,
                                          sizeof(SurfaceIndex),
                                          output_surface_idx));
    cm_result_check(kernel2->SetKernelArg(1,
                                          4,
                                          &num_sum_threads));

    // Creates a CmTask object.
    // The CmTask object is a container for CmKernel pointers. It is used to
    // enqueue the kernels for execution.
    cm_result_check(device->CreateTask(task));

    // Adds a CmKernel pointer to CmTask.
    cm_result_check(task->AddKernel(kernel2));

    // Launches the task on the GPU. Enqueue is a non-blocking call, i.e. the
    // function returns immediately without waiting for the GPU to start or
    // finish execution of the task. The runtime will query the HW status. If
    // the hardware is not busy, the runtime will submit the task to the
    // driver/HW; otherwise, the runtime will submit the task to the driver/HW
    // at another time.
    cm_result_check(cmd_queue->Enqueue(task, sync_event));

    // Destroys a CmTask object.
    // CmTask will be destroyed when CmDevice is destroyed.
    // Here, the application destroys the CmTask object by itself.
    cm_result_check(device->DestroyTask(task));

    // Reads the output surface content to the system memory using the CPU.
    // The size of data copied is the size of data in Surface.
    // It is a blocking call. The function will not return until the copy
    // operation is completed.
    // The dependent event "sync_event" ensures that the reading of the surface
    // will not happen until its state becomes CM_STATUS_FINISHED.
    cm_result_check(output_surface->ReadSurface((unsigned char *)hist,
                                                sync_event));

    // Destroys the CmDevice.
    // Also destroys surfaces, kernels, tasks, thread spaces, and queues that
    // were created using this device instance that have not explicitly been
    // destroyed by calling the respective destroy functions.
    cm_result_check(::DestroyCmDevice(device));

    // Checks result.
    // If the total point = 65536 the result is correct.
    // Or else there is something wrong.
    if (WriteHist(hist) == 65536) {
        printf("PASSED\n");
        return 0;
    } else {
        printf("FAILED\n");
        return -1;
    }
  
    // Frees memory.
    delete[] src_image;
    delete[] hist;
}