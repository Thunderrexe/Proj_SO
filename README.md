# cloudIST

CFLAG NO MAKEFILE (MacOS): -std=gnu2x

Acrescentar isto no Makefile para suportar threads.

CFLAGS += -pthread

Acrescentar isto no Makefile para suportar o ficheiro adicional (vm_process.o).

cloudIST: main.c constants.h parser.o datacenter.o datacenter_utils.o resources.o filesystem.o vm_process.o
	$(CC) $(CFLAGS) $(SLEEP) -o cloudIST main.c parser.o datacenter.o datacenter_utils.o resources.o filesystem.o vm_process.o -lm


This repository contains implementation for the cloudIST operating systems project. 
The program simulates a data center that manages servers, VM types, reservations, and VM execution commands.

## Project Structure

### Core source files

- `main.c`: Program entry point, command loop, and command-line configuration.
- `parser.c` / `parser.h`: Parsing of command-line values and commands read from
  standard input.
- `datacenter.c` / `datacenter.h`: Data center configuration, VM type
  registration, reservations, execution, listing, and waits.
- `datacenter_utils.c` / `datacenter_utils.h`: VM type and reservation lookup,
  reservation validation and allocation, cleanup, and VM lifecycle helpers.
- `resources.c` / `resources.h`: Resource operations for CPU, RAM, and disk.
- `filesystem.c` / `filesystem.h`: Filesystem checks and absolute-path
  resolution.
- `constants.h`: Limits used by the data center, reservations, VM types, and
  identifiers.


- Inputs no Mac

D V1 ./inputs_vm/v1 /bin/ls 2 50 2
D V2 ./inputs_vm/v2 /bin/ls 4 100 4
R res_teste V1 2 1 V2 1 1
L
E 5000
