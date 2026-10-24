#ifndef VM_PROCESS_H
#define VM_PROCESS_H

#include "datacenter_utils.h"

void spawn_vm_child(VM *vm,size_t total_physical_cpus,size_t local_cores);

void rollback_spawned_vms(VM **started, size_t started_count);

/**
 * Forks one child per VM in the reservation, calling spawn_vm_child()
 * in each child and recording the pid/state in the parent.
 *
 * @param res Pointer to the Reservation whose VMs should be spawned.
 *
 * @return 0 on success.
 * @return 1 if any fork() call fails.
 */
int spawn_all_vms(Reservation *res,size_t total_physical_cpus,size_t local_cores);


int reap_finished_vms(DataCenter *dc);

int vm_destroy(Reservation *reservation, VM *vm);

#endif