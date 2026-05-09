A high-performance line rate L2 packet reflector.

Develop by NVIDIA DOCA Architecture and BlueField-3 DPUs.

Optimizing designs:

1. Multithreads. Using sufficient (256) DPA cores to process packet reflecting in parallel. Also designed a routing module for load balance across multiple threads.

2. Light weighted fencing. Using thread-level data fence instead of system-wise fence, while maintaining data correctiveness.

3. Control-data path disaggregation. DPA can only direclty access dedicated SRAM, which has only 4MB of capacity. This capacity is not sufficient for line-rate packet caching. Observed that the control plane: DPA only read and modity the packet head (64B), we designed a control data and payload disaggregated storaging archtecture. The SRAM of DPA only storage the packet head, while the payload part is stored in the DRAM (16GB) of DPU. Although the DRAM cannot be directly accessed by DPA, both SRAM dna DRAM can be accessed by mlx5 NIC logics. After DPA finished the modification of packet head, it doorbells the NIC and NIC reads packet head from SRAM and payload from DRAM, assemly them and fire to the network.

4. Evaluation: 382GB/s of reflecting rate using 64 DPA threads in a 400GB/s BlueField-3 DPU.
