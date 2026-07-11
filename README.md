# AES-128 CTR FPGA Accelerator with STM32 SPI Interface

This project presents a hardware implementation of the AES-128 encryption algorithm in Counter (CTR) mode on a Spartan-6 FPGA, controlled by an STM32 microcontroller through an SPI interface.

The design was developed as an embedded hardware acceleration system in which the microcontroller prepares the input data, key, and initialization vector (IV/counter), sends them to the FPGA, and receives the encrypted output.

## Overview

The system consists of two main parts:

- **FPGA side**: AES-128 CTR hardware core implemented in Verilog/VHDL
- **Microcontroller side**: STM32 firmware for SPI communication and test execution

The FPGA performs AES-128 block encryption and generates the CTR-mode keystream, while the STM32 acts as the host controller for data transfer and result verification.

## Features

- AES-128 encryption in CTR mode
- Iterative hardware architecture for reduced area usage
- SPI-based communication between STM32 and FPGA
- Practical implementation on Spartan-6 FPGA
- Functional verification using simulation and real hardware testing
- NIST-compatible AES/CTR verification flow

## Hardware Platform

- **FPGA**: Xilinx Spartan-6
- **Microcontroller**: STM32F401CEU6
- **Communication Interface**: SPI
- **Voltage Standard**: LVCMOS33

## Project Structure

Suggested repository structure:
```text
rtl/        FPGA source files
tb/         Testbenches and simulation files
ucf/        Pin constraint files
mcu/        STM32 firmware
docs/       Images, reports, or documentation

## FPGA Design Description

The FPGA implementation includes:

- AES-128 encryption core
- Key expansion unit
- CTR control logic
- SPI interface logic
- Top-level integration module

The AES core follows an iterative architecture, where one round of AES processing is reused across multiple clock cycles. This reduces hardware cost compared to fully unrolled architectures, at the expense of throughput.

## Microcontroller Role

The STM32 firmware is responsible for:

- Initializing peripherals
- Configuring and controlling SPI communication
- Sending the secret key and IV/counter to the FPGA
- Sending plaintext data blocks
- Receiving ciphertext output
- Printing or logging test results for verification

## AES-CTR Operation

In CTR mode, the FPGA encrypts the counter block using AES-128 and produces a keystream block. The plaintext is XORed with this keystream to generate the ciphertext.

For each new block:
1. Counter value is loaded or incremented
2. AES encryption is applied to the counter block
3. Generated keystream is XORed with plaintext
4. Ciphertext is returned to the STM32

## Verification

The design was verified in two stages:

### 1. Simulation
The FPGA core was simulated using dedicated testbenches and validated against known AES/CTR test vectors.

### 2. Hardware Test
The complete system was tested in practice using an STM32 microcontroller connected to the FPGA through SPI. Input parameters such as key, IV, plaintext, and ciphertext were monitored to confirm correct system behavior.

## Example Test Parameters

- **Key**: `2b7e151628aed2a6abf7158809cf4f3c`
- **IV**: `f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff`

Example AES-CTR test vectors and expected behavior were compared with standard references.

## Tools Used

- Xilinx ISE Design Suite
- FPGA simulation tools compatible with Spartan-6 flow
- STM32 development environment (such as STM32CubeIDE or equivalent)
- Serial terminal for output monitoring

## References

Key references used in this project include:

- NIST FIPS 197: Advanced Encryption Standard (AES)
- NIST SP 800-38A: Recommendation for Block Cipher Modes of Operation
- RFC 3686: Using AES Counter Mode with IPsec ESP
- Xilinx Spartan-6 documentation
- STM32F401 reference manual and datasheet

## Notes

- This repository focuses on the implementation and validation of AES-128 in CTR mode.
- The design emphasizes practical hardware realization and embedded interfacing rather than maximum throughput optimization.
- The project can be extended to support AES-192, AES-256, decryption, or more optimized parallel architectures.

## Author

Mohammad Hossein Ahmadi

## License

This project is provided for educational and academic use.
