#include <assert.h>

#include "host_api.hpp"
#include "device.hpp"

#define DATA_SIZE 100

using namespace tt;
using namespace tt::tt_metal;

int main(int argc, char** argv) {
    // Create device handle
    IDevice* device = CreateDevice(0);

    // Setup program to execute along with its buffers and kernels to use
    CommandQueue& cq = device->command_queue();
    Program program = CreateProgram();
    constexpr CoreCoord core = {0, 0};

    // Create descriptor of DRAM allocation
    constexpr uint32_t single_tile_size = 4 * DATA_SIZE;
    InterleavedBufferConfig dram_config{
        .device = device, .size = single_tile_size, .page_size = single_tile_size, .buffer_type = BufferType::DRAM};

    // Use descriptor configuration to allocate buffers in DRAM on the device
    std::shared_ptr<Buffer> src0_dram_buffer = CreateBuffer(dram_config);
    std::shared_ptr<Buffer> src1_dram_buffer = CreateBuffer(dram_config);
    std::shared_ptr<Buffer> dst_dram_buffer = CreateBuffer(dram_config);

    tt::tt_metal::InterleavedBufferConfig l1_config {
        .device= device,
        .size = single_tile_size,
        .page_size = single_tile_size,
        .buffer_type = tt::tt_metal::BufferType::L1 };

    std::shared_ptr<tt::tt_metal::Buffer> l1_buffer_1 = CreateBuffer(l1_config);
    std::shared_ptr<tt::tt_metal::Buffer> l1_buffer_2 = CreateBuffer(l1_config);
    
    // Create L1 circular buffers to communicate between RV in and RV out cores
    constexpr uint32_t src0_cb_index = CBIndex::c_0;
    CircularBufferConfig cb_src0_config =
        CircularBufferConfig(single_tile_size, {{src0_cb_index, tt::DataFormat::UInt32}})
            .set_page_size(src0_cb_index, single_tile_size);
    CBHandle cb_src0 = tt_metal::CreateCircularBuffer(program, core, cb_src0_config);

    // Allocate input data and fill it with values (each will be added together)
    uint32_t * src0_data=(uint32_t*) malloc(sizeof(uint32_t) * DATA_SIZE);
    uint32_t * src1_data=(uint32_t*) malloc(sizeof(uint32_t) * DATA_SIZE);

    for (int i=0;i<DATA_SIZE;i++) {
        src0_data[i]=i;
        src1_data[i]=DATA_SIZE-i;
    }

    // Write the src0 and src1 data to DRAM on the device, false means we will not block
    EnqueueWriteBuffer(cq, src0_dram_buffer, src0_data, false);
    EnqueueWriteBuffer(cq, src1_dram_buffer, src1_data, false);

    // Specify data movement kernel for launching on first RISC-V baby core
    KernelHandle reader_kernel_id = CreateKernel(
        program,
        "kernels/dataflow/read_kernel.cpp",
        core,
        DataMovementConfig{.processor = DataMovementProcessor::RISCV_0, .noc = NOC::RISCV_0_default});

    // Configure reader runtime kernel arguments
    SetRuntimeArgs(
        program,
        reader_kernel_id,
        core,
        {src0_dram_buffer->address(),
         src1_dram_buffer->address(),
         l1_buffer_1->address(),
         l1_buffer_2->address(),
         DATA_SIZE});

    // Specify data movement kernel for launching on last RISC-V baby core
    KernelHandle writer_kernel_id = CreateKernel(
        program,
        "kernels/dataflow/write_kernel.cpp",
        core,
        DataMovementConfig{.processor = DataMovementProcessor::RISCV_1, .noc = NOC::RISCV_1_default});

    // Configure writer runtime kernel arguments
    SetRuntimeArgs(
        program,
        writer_kernel_id,
        core,
        {dst_dram_buffer->address(),
         DATA_SIZE});

    // Enqueue the program to run on the device, false means will not block    
    EnqueueProgram(cq, program, false);
    // Wait on the command queue to complete
    Finish(cq);

    // Allocate result data on host for results
    uint32_t * result_data=(uint32_t*) malloc(sizeof(uint32_t) * DATA_SIZE);
    // Copy results back from device DRAM, true means will block for completion
    EnqueueReadBuffer(cq, dst_dram_buffer, result_data, true);
    // Check all results match expected value
    int number_failures=0;
    for (int i=0;i<DATA_SIZE;i++) {
        if (result_data[i] != src0_data[i] + src1_data[i]) number_failures++;
    }

    CloseDevice(device);

    if (number_failures==0) {
        printf("Completed successfully on the device, with %d elements\n", DATA_SIZE);
    } else {
        printf("Failure on the device, %d fails with %d elements\n", number_failures, DATA_SIZE);
    }
}

