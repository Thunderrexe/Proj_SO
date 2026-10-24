#include "datacenter_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>

VMType *VMType_exists(DataCenter *dc, const char* type_id){
	for(size_t i = 0; i < dc->num_vm_types; i++){
		if(strncmp(type_id, dc->vm_types[i].id, MAX_STRING_SIZE) == 0){
			return &dc->vm_types[i];
		}
	}
	return NULL;
}

/**
 * Checks if a reservation with the given id already exists in the Data Center.
 * 
 * @param dc Pointer to Data Center.
 * @param reservation_id Id of the reservation being checked.
 * 
 * @return 1 if the reservation exists.
 * @return 0 if the reservation does not exist.
 */	
static int reservation_exists(DataCenter *dc, const char* reservation_id){
	for(size_t i = 0; i < dc->num_reservations; i++){
		if(strncmp(reservation_id, dc->reservations[i].id, MAX_STRING_SIZE) == 0){
			return 1;
		}
	}
	return 0;
}

int reservation_validate(DataCenter *dc, Reservation *reservation) {
	if(reservation_exists(dc, reservation->id)){
		fprintf(stderr, "Reservation with id %s already exists.\n", reservation->id);
		return 1;
	}

	Resources available[dc->num_servers];
	size_t hosted_count[dc->num_servers];
	size_t vms_count = 0;

	/* Copy current server resources and hosted_vms so we can simulate the allocation. */
	for (size_t i = 0; i < dc->num_servers; i++) {
		available[i] = dc->servers[i].available;
		hosted_count[i] = dc->servers[i].num_hosted_vms;
	}

	for (size_t i = 0; i < reservation->items_count; i++) {
		ReservationItem *item = &reservation->items[i];

		if (item->amount == 0) {
			fprintf(stderr, "Reservation item must request at least one VM.\n");
			return 1;
		}

		VMType *vm_type = VMType_exists(dc, item->vm_type_id);

		if(vm_type == NULL){
			fprintf(stderr, "VM of type %s does not exist\n", item->vm_type_id);
			return 1;
		}

		if (item->server_id == 0 || item->server_id > dc->num_servers) {
			fprintf(stderr, "Invalid server in reservation.\n");
			return 1;
		}

		for (size_t j = 0; j < item->amount; j++) {
			if(vms_count >= MAX_RESERVATION_VMS){
				fprintf(stderr, "Reached max of VMS for a reservation.\n");
				return 1;
			}
			
			if (hosted_count[item->server_id - 1] >= MAX_HOSTED_VMS) {
        fprintf(stderr,
                "Server \"%zu\" reached the maximum number of hosted VMs.\n",
                item->server_id);
        return 1;
    	}

			if (!resources_can_fit(available[item->server_id-1], vm_type->required)) {
				fprintf(stderr, "Server \"%zu\" does not have enough resources.\n", item->server_id);
				return 1;
			}

			resources_sub(&available[item->server_id-1], vm_type->required);
			hosted_count[item->server_id-1]++;
			vms_count++; // One VM "reserved"
		}

		// Attach vm_type to item for later use.
		item->vm_type = vm_type;
	}

	return 0;
}

/**
 * Rolls back the changes made by a reservation operation.
 * 
 * @param reservation Pointer to the reservation being rolled back.
 * @param initial_vms Number of VMs that existed before the operation.
 */
static void reservation_rollback(Reservation *reservation, size_t initial_vms){
	while (reservation->num_vms > initial_vms) {
		VM *vm = reservation->vms[reservation->num_vms - 1];

		Server *server = vm->server;

		// Restore resources
		resources_add(&server->available, vm->type->required);

		// Remove VM from server hosted list
		server->num_hosted_vms--;

		server->hosted_vms[server->num_hosted_vms] = NULL;

		// Remove VM from reservation
		reservation->num_vms--;

		reservation->vms[reservation->num_vms] = NULL;

		free(vm);
	}
}

static int copy_file(const char *src_path, const char *dst_path) {
    int src_fd = open(src_path, O_RDONLY);
    if (src_fd < 0) {
        return -1;
    }

    int dst_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (dst_fd < 0) {
        close(src_fd);
        return -1;
    }

    char buffer[4096];
    ssize_t bytes_read;
    int result = 0;

    while ((bytes_read = read(src_fd, buffer, sizeof(buffer))) > 0) {
        ssize_t total_written = 0;

        while (total_written < bytes_read) {
            ssize_t bytes_written = write(
                dst_fd,
                buffer + total_written,
                (size_t)(bytes_read - total_written)
            );

            if (bytes_written <= 0) {
                result = -1;
                break;
            }

            total_written += bytes_written;
        }

        if (result == -1) {
            break;
        }
    }

    if (bytes_read < 0) {
        result = -1;
    }

    if (close(src_fd) < 0) {
        result = -1;
    }

    if (close(dst_fd) < 0) {
        result = -1;
    }

    return result;
}

static int copy_directory_recursive(const char *src, const char *dst) {
    DIR *dir = opendir(src);
    if (dir == NULL) {
        return -1;
    }

    struct stat st;
    if (stat(dst, &st) != 0) {
        if (mkdir(dst, 0777) != 0) {
            closedir(dir);
            return -1;
        }
    } else if (!S_ISDIR(st.st_mode)) {
        closedir(dir);
        return -1;
    }

    int result = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char src_path[MAX_PATH_SIZE];
        char dst_path[MAX_PATH_SIZE];

        int src_len = snprintf(
            src_path, sizeof(src_path), "%s/%s", src, entry->d_name
        );
        int dst_len = snprintf(
            dst_path, sizeof(dst_path), "%s/%s", dst, entry->d_name
        );

        if (src_len < 0 || (size_t)src_len >= sizeof(src_path) ||
            dst_len < 0 || (size_t)dst_len >= sizeof(dst_path)) {
            result = -1;
            break;
        }

        struct stat entry_stat;
        if (stat(src_path, &entry_stat) != 0) {
            result = -1;
            break;
        }

        int copy_result;

        if (S_ISDIR(entry_stat.st_mode)) {
            copy_result = copy_directory_recursive(src_path, dst_path);
        } else if (S_ISREG(entry_stat.st_mode)) {
            copy_result = copy_file(src_path, dst_path);
        } else {
            continue;
        }

        if (copy_result != 0) {
            result = -1;
            break;
        }
    }

    if (closedir(dir) != 0) {
        result = -1;
    }

    return result;
}

//Função principal para criar a pasta da VM e copiar os inputs
static int setup_vm_filesystem(
    const char *reservation_id,
    const char *vm_id,
    const char *input_folder
) {
    const char *base_path = "/tmp/CloudIST";
    char reservation_path[MAX_PATH_SIZE];
    char vm_path[MAX_PATH_SIZE];

    struct stat st;

    if (stat(base_path, &st) != 0) {
        if (mkdir(base_path, 0777) != 0) {
            return -1;
        }
    } else if (!S_ISDIR(st.st_mode)) {
        return -1;
    }

    int reservation_len = snprintf(
        reservation_path,
        sizeof(reservation_path),
        "%s/%s",
        base_path,
        reservation_id
    );

    if (reservation_len < 0 ||
        (size_t)reservation_len >= sizeof(reservation_path)) {
        return -1;
    }

    if (stat(reservation_path, &st) != 0) {
        if (mkdir(reservation_path, 0777) != 0) {
            return -1;
        }
    } else if (!S_ISDIR(st.st_mode)) {
        return -1;
    }

    int vm_len = snprintf(
        vm_path,
        sizeof(vm_path),
        "%s/%s",
        reservation_path,
        vm_id
    );

    if (vm_len < 0 || (size_t)vm_len >= sizeof(vm_path)) {
        return -1;
    }

    if (mkdir(vm_path, 0777) != 0) {
        return -1;
    }

    return copy_directory_recursive(input_folder, vm_path);
}

int reservation_commit(DataCenter *dc, Reservation *reservation){
	// Keep track of how many vms existed before.
	size_t initial_vms = reservation->num_vms;

	for (size_t i = 0; i < reservation->items_count; i++) {
		ReservationItem *item = &reservation->items[i];
		Server *server = &dc->servers[item->server_id - 1];

		for (size_t j = 0; j < item->amount; j++) {
			VM *new_vm = calloc(1, sizeof(VM));
			if (new_vm == NULL) {
				fprintf(stderr, "Failed to allocate VM.\n");
				reservation_rollback(reservation, initial_vms);
				return 1;
			}

			snprintf(new_vm->id,
				MAX_VM_ID_STRING,
				"%s_%zu",
				reservation->id,
				reservation->num_vms + 1
			);

			
			new_vm->type = item->vm_type;
			new_vm->server = server;
			new_vm->state = VM_STATE_RESERVED;

			if (setup_vm_filesystem(reservation->id, new_vm->id, item->vm_type->input_folder) != 0) {
    			fprintf(stderr, "Failed to set up filesystem for VM %s.\n", new_vm->id);
    			free(new_vm);
    			reservation_rollback(reservation, initial_vms);
    			return 1;
			}

			resources_sub(&server->available, item->vm_type->required);

			server->hosted_vms[server->num_hosted_vms++] = new_vm;
			reservation->vms[reservation->num_vms++] = new_vm;
		}
	}

	reservation->state = RES_STATE_PENDING;

	return 0;
}

Reservation *find_pending_reservation(DataCenter *dc, const char *reservation_id) {
	Reservation *res = NULL;

	for (size_t i = 0; i < dc->num_reservations; i++) {
		if (strcmp(dc->reservations[i].id, reservation_id) == 0) {
			res = &dc->reservations[i];
			break;
		}
	}

	if (!res) {
		fprintf(stderr, "Reservation ID %s not found.\n", reservation_id);
		return NULL;
	}

	if (res->state != RES_STATE_PENDING) {
		fprintf(stderr, "Reservation ID %s is not in a pending state.\n", reservation_id);
		return NULL;
	}

	return res;
}

int remove_path_recursive(const char *path) {
    struct stat st;

    /*
     * lstat() permite reconhecer ligações simbólicas
     * sem seguir o seu destino.
     */
    if (lstat(path, &st) == -1) {
        if (errno == ENOENT) {
            /* O caminho já não existe: limpeza concluída. */
            return 0;
        }

        return -1;
    }

    /*
     * Se não for uma diretoria, basta remover a entrada.
     * Isto inclui ficheiros e ligações simbólicas.
     */
    if (!S_ISDIR(st.st_mode)) {
        return unlink(path);
    }

    DIR *dir = opendir(path);

    if (dir == NULL) {
        return -1;
    }

    int result = 0;
    struct dirent *entry;

    while (1) {
        errno = 0;
        entry = readdir(dir);

        if (entry == NULL) {
            if (errno != 0) {
                result = -1;
            }

            break;
        }

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char child_path[MAX_PATH_SIZE];

        int n = snprintf(child_path,
                         sizeof(child_path),
                         "%s/%s",
                         path,
                         entry->d_name);

        if (n < 0 || (size_t)n >= sizeof(child_path)) {
            result = -1;
            continue;
        }

        if (remove_path_recursive(child_path) != 0) {
            result = -1;
        }
    }

    if (closedir(dir) != 0) {
        result = -1;
    }

    /*
     * Só é possível remover a diretoria depois de limpar
     * as entradas que contém.
     */
    if (rmdir(path) != 0) {
        result = -1;
    }

    return result;
}

void reservation_destroy(DataCenter *dc, Reservation *reservation) {
	// Free every VM belonging to the reservation.
	for (size_t i = 0; i < reservation->num_vms; i++) {
		VM *vm = reservation->vms[i];
		Server *server = vm->server;

		// Return the VM resources back to the hosting server.
		resources_add(&server->available, vm->type->required);

		// Remove the VM from the server's hosted VM list.
		for (size_t j = 0; j < server->num_hosted_vms; j++) {
			if (server->hosted_vms[j] == vm) {
				// Shift the remaining VMs one position to the left.
				memmove(&server->hosted_vms[j],
								&server->hosted_vms[j + 1],
								(server->num_hosted_vms - j - 1) * sizeof(VM *));

				server->num_hosted_vms--;
				break;
			}
		}

		free(vm);
		reservation->vms[i] = NULL;
	}

	reservation->num_vms = 0;

	// Find the reservation inside the Data Center reservation list.
	size_t idx;
	for (idx = 0; idx < dc->num_reservations; idx++) {
			if (&dc->reservations[idx] == reservation)
				break;
	}

	// Reservation not found (should never happen).
	if (idx == dc->num_reservations)
		return;

    /*
     * Eliminar a diretoria da reserva e todo o seu conteúdo.
     */
    char reservation_path[MAX_PATH_SIZE];

    int n = snprintf(reservation_path,
                     sizeof(reservation_path),
                     "/tmp/CloudIST/%s",
                     reservation->id);

    if (n < 0 || (size_t)n >= sizeof(reservation_path)) {
        fprintf(stderr, "Reservation path too long.\n");
        return;
    }

    if (remove_path_recursive(reservation_path) != 0) {
        /*
         * A limpeza do sistema de ficheiros falhou,
         * mas devemos continuar a libertar o registo
         * da reserva durante a destruição final.
         */
        perror("Failed to remove reservation directory");
    }

	// Remove the reservation by shifting the remaining ones.
	memmove(&dc->reservations[idx],
					&dc->reservations[idx + 1],
					(dc->num_reservations - idx - 1) * sizeof(Reservation));

	dc->num_reservations--;
}

int remove_completed_reservation(DataCenter *dc, Reservation *res) {
    
    if (dc == NULL || res == NULL || res->num_vms != 0) {
        fprintf(stderr, "Invalid completed reservation.\n");
        return 1;
    }

    /*
     * Encontrar a posição da reserva no array do centro de dados.
     */
    size_t idx = 0;

    while (idx < dc->num_reservations &&
           &dc->reservations[idx] != res) {
        idx++;
    }

    if (idx == dc->num_reservations) {
        fprintf(stderr, "Reservation not found.\n");
        return 1;
    }

    /*
     * Remover a diretoria da reserva e tudo o que ainda contenha.
     */
    char reservation_path[MAX_PATH_SIZE];

    int n = snprintf(reservation_path,sizeof(reservation_path),"/tmp/CloudIST/%s",res->id);

    if (n < 0 || (size_t)n >= sizeof(reservation_path)) {
        fprintf(stderr, "Reservation path too long.\n");
        return 1;
    }

    if (remove_path_recursive(reservation_path) != 0) {
        perror("Failed to remove reservation directory");
        return 1;
    }

    /*
     * Remover a reserva do array.
     */
    size_t remaining = dc->num_reservations - idx - 1;

    if (remaining > 0) {
        memmove(&dc->reservations[idx],
                &dc->reservations[idx + 1],
                remaining * sizeof(dc->reservations[0]));
    }

    dc->num_reservations--;

    /*
     * Limpar a última posição, que deixou de ser utilizada.
     */
    memset(&dc->reservations[dc->num_reservations],
           0,
           sizeof(dc->reservations[0]));

    return 0;
}

const char *vm_state_to_string(VMState state) {
    switch (state) {
        case VM_STATE_RESERVED:   return "RESERVED";
        case VM_STATE_RUNNING:    return "RUNNING";
        case VM_STATE_TERMINATED: return "TERMINATED";
        default:                  return "UNKNOWN";
    }
}

const char *res_state_to_string(ReservationState state) {
    switch (state) {
			case RES_STATE_PENDING:  return "PENDING";
			case RES_STATE_RUNNING:  return "RUNNING";
			case RES_STATE_FINISHED: return "FINISHED";
			default:                 return "UNKNOWN";
    }
}