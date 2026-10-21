#include "datacenter.h"
#include "datacenter_utils.h"

#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>


void datacenter_init(DataCenter *dc) {
  dc->servers = NULL;
  dc->num_servers = 0;

  dc->num_vm_types = 0;

  dc->num_reservations = 0;

  dc->configured = 0;
}

void datacenter_destroy(DataCenter *dc) {
  if (dc == NULL) {
    return;
  }

  /*
   * As aplicações devem ter terminado e os filhos devem
   * ter sido recolhidos antes de destruir o DataCenter.
   */
  for (size_t i = 0; i < dc->num_reservations; i++) {
    Reservation *res = &dc->reservations[i];
    for (size_t j = 0; j < res->num_vms; j++) {
      VM *vm = res->vms[j];
      if (vm != NULL && vm->state == VM_STATE_RUNNING) {
        fprintf(stderr, "Cannot destroy DataCenter: VM %s is still running.\n", vm->id);
        return;
      }
    }
  }

  /*
   * Destruir todas as reservas.
   * reservation_destroy() liberta as VMs, devolve os
   * recursos e remove cada reserva da lista.
   */
  while (dc->num_reservations > 0) {
    size_t previous_count = dc->num_reservations;
    Reservation *res = &dc->reservations[dc->num_reservations - 1];
    reservation_destroy(dc, res);
    /*
     * Evitar um ciclo infinito caso a reserva
     * não tenha sido removida.
     */
    if (dc->num_reservations == previous_count) {
      fprintf(stderr,"Failed to destroy reservation during cleanup.\n");
      return;
    }
  }

  /*
   * As VMs já foram removidas das listas dos servidores
   * por reservation_destroy(). Só falta libertar o array.
   */
  free(dc->servers);
  dc->servers = NULL;
  dc->num_servers = 0;
  dc->configured = 0;
}

int datacenter_configure(DataCenter *dc, size_t num_servers, Resources *resources){
  if(dc->configured){
    fprintf(stderr, "Data Center is already configured!\n");
    return 1;
  }

  if (num_servers == 0 || resources->ram == 0 || resources->disk == 0 || resources->cpu == 0) {
    fprintf(stderr, "Invalid configuration values.\n");
    return 1;
  }

  dc->servers = (Server*)malloc(num_servers*sizeof(Server));

  if(dc->servers == NULL){
    fprintf(stderr, "Failed to allocate memory for servers.\n");
    return 1;
  }

  for (size_t i = 0; i < num_servers; i++) {
    dc->servers[i].id = i+1;
    dc->servers[i].total = *resources;
    dc->servers[i].available = *resources;
    dc->servers[i].num_hosted_vms = 0;
  }

  dc->num_servers = num_servers;
  dc->configured = 1;

  return 0;
}

int datacenter_define_VM(DataCenter *dc, VMType *type){
  if (!dc->configured) {
    fprintf(stderr, "Data Center needs to be configured!\n");
    return 1;
  }

  if(VMType_exists(dc, type->id) != NULL){
    fprintf(stderr, "VM type with id %s already exists.\n", type->id);
    return 1;
  }

  if (!file_exists(type->exec_path)) {
    fprintf(stderr, "Invalid executable path.\n");
    return 1;
  }

  if (!path_exists(type->input_folder)) {
    fprintf(stderr, "Invalid input folder.\n");
    return 1;
  }

  if(dc->num_vm_types >= MAX_VM_TYPES){
    fprintf(stderr, "Maximum of VM types reached.\n");
    return 1;
  }
  
  dc->vm_types[dc->num_vm_types++] = *type;

  return 0;
}

int datacenter_reserve(DataCenter *dc, Reservation *reservation) {
  if (!dc->configured) {
    fprintf(stderr, "Data Center needs to be configured!\n");
    return 1;
  }

  if(reservation_validate(dc, reservation) != 0){
    return 1;
  }

  if (dc->num_reservations >= MAX_RESERVATIONS){
    fprintf(stderr, "Max number of reservations reached\n");
    return 1;
  }

  if (reservation_commit(dc, reservation) != 0)
    return 1;

  dc->reservations[dc->num_reservations] = *reservation;
  dc->num_reservations++;

  return 0;
}

int datacenter_execute(DataCenter *dc, const char *reservation_id, size_t total_physical_cpus,size_t local_cores) {
    if (!dc->configured) {
        fprintf(stderr, "Data Center needs to be configured!\n");
        return 1;
    }

    Reservation *res =
        find_pending_reservation(dc, reservation_id);

    if (res == NULL) {
        return 1;
    }

    if (spawn_all_vms(res, total_physical_cpus, local_cores) != 0) {
        fprintf(stderr, "Failed to start reservation.\n");
        return 1;
    }

    /*
     * spawn_all_vms() já altera o estado da reserva
     * para RES_STATE_RUNNING quando termina com sucesso.
     *
     * Não esperamos pelas aplicações nesta função.
     * A monitorização e a limpeza serão tratadas
     * posteriormente.
     */

    return 0;
}

int datacenter_list(DataCenter *dc) {
  if (!dc->configured) {
    fprintf(stderr, "Data Center needs to be configured!\n");
    return 1;
  }

  printf("=================================== DATA CENTER STATUS ===================================\n");
  printf("\n[ PHYSICAL SERVERS ] (%zu total)\n", dc->num_servers);
  printf("------------------------------------------------------------------------------------------\n");

  for (size_t i = 0; i < dc->num_servers; i++) {
    Server server = dc->servers[i];

    Resources used = server.total;
    resources_sub(&used, server.available);

    printf(" Server #%zu\n", server.id);
    printf("   ├─ Resources Total     : %6zu vCPUs | %3zu GB RAM | %3zu GB Disk\n",
            server.total.cpu, server.total.ram, server.total.disk);
    printf("   ├─ Resources Used      : %6zu vCPUs | %3zu GB RAM | %3zu GB Disk\n",
            used.cpu, used.ram, used.disk);
    printf("   ├─ Resources Available : %6zu vCPUs | %3zu GB RAM | %3zu GB Disk\n",
            server.available.cpu, server.available.ram, server.available.disk);
    printf("   └─ Hosted VMs (%zu):\n", server.num_hosted_vms);

    if (server.num_hosted_vms == 0) {
      printf("        (None)\n");
    } else {
      for (size_t j = 0; j < server.num_hosted_vms; j++) {
        VM *vm = server.hosted_vms[j];
        const char *prefix = (j == server.num_hosted_vms - 1) ? "└──" : "├──";

        printf("        %s VM ID : %-16s | Type: %-10s | State: %-10s",
                prefix,
                vm->id,
                vm->type ? vm->type->id : "Unknown",
                vm_state_to_string(vm->state));

        if (vm->state == VM_STATE_RUNNING) {
          printf(" | PID: %d", (int)vm->pid);
        }
        printf("\n");
      }
    }
    printf("\n");
  }

  printf("[ DEFINED VM TYPES ] (%zu registered)\n", dc->num_vm_types);
  printf("------------------------------------------------------------------------------------------\n");
  if (dc->num_vm_types == 0) {
    printf(" (None)\n\n");
  } else {
      for (size_t i = 0; i < dc->num_vm_types; i++) {
        VMType vt = dc->vm_types[i];
        printf("  * Type ID: %-12s | Requirements: %.2zu vCPU, %zu GB RAM, %zu GB Disk\n",
                vt.id, vt.required.cpu, vt.required.ram, vt.required.disk);
        printf("    ├─ Executable : %s\n", vt.exec_path);
        printf("    └─ Input Dir  : %s\n\n", vt.input_folder);
      }
      printf("\n");
  }

  printf("[ RESERVATIONS ] (%zu total)\n", dc->num_reservations);
  printf("------------------------------------------------------------------------------------------\n");
  if (dc->num_reservations == 0) {
    printf(" (None)\n\n");
  } else {
    for (size_t i = 0; i < dc->num_reservations; i++) {
      Reservation res = dc->reservations[i];
      printf("  * Reservation ID: %-12s | State: %-8s | Total VMs: %zu\n",
              res.id, res_state_to_string(res.state), res.num_vms);

      printf("    ├─ Requested Pairs:\n");
      for (size_t k = 0; k < res.items_count; k++) {
        printf("    │    • %-12s : %zu unit(s)\n", res.items[k].vm_type_id, res.items[k].amount);
      }

      printf("    └─ Allocated VMs:\n");
      if (res.num_vms == 0) {
        printf("         (None)\n");
      } else {
          for (size_t k = 0; k < res.num_vms; k++) {
            VM *vm = res.vms[k];
            const char *prefix = (k == res.num_vms - 1) ? "└──" : "├──";
            printf("         %s ID: %-15s | Host Server: #%zu\n",
                    prefix,
                    vm->id,
                    vm->server->id);
          }
      }
      printf("\n");
    }
  }

  printf("================================================================--------------------------\n\n");
  return 0;
}

void datacenter_wait(DataCenter *dc, unsigned int delay_ms) {
    const unsigned int poll_interval_ms = 10;
    unsigned int elapsed_ms = 0;

    while (elapsed_ms < delay_ms) {
        unsigned int chunk_ms = delay_ms - elapsed_ms;

        if (chunk_ms > poll_interval_ms) {
            chunk_ms = poll_interval_ms;
        }

        struct timespec request = {
            .tv_sec = chunk_ms / 1000,
            .tv_nsec = (long)(chunk_ms % 1000) * 1000000L
        };

        struct timespec remaining;

        /*
         * Continuar a dormir se nanosleep for interrompido
         * por um sinal.
         */
        while (nanosleep(&request, &remaining) == -1) {
            if (errno == EINTR) {
                request = remaining;
                continue;
            }

            perror("nanosleep");
            return;
        }

        elapsed_ms += chunk_ms;

        /*
         * Verificar processos terminados durante a espera.
         */
        if (reap_finished_vms(dc) != 0) {
            fprintf(stderr,
                    "Warning: errors while processing terminated VMs.\n");
        }
    }
}