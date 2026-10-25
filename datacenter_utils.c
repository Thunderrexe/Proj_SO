#include "datacenter_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>

/*
 * Procura um tipo de VM pelo identificador.
 * Devolve um ponteiro para o tipo existente ou NULL se não for encontrado.
 */
VMType *VMType_exists(DataCenter *dc, const char* type_id){
	for(size_t i = 0; i < dc->num_vm_types; i++){
		if(strncmp(type_id, dc->vm_types[i].id, MAX_STRING_SIZE) == 0){
			return &dc->vm_types[i];
		}
	}
	return NULL;
}

/*
 * Verifica se já existe uma reserva com o identificador indicado.
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

    /*
     * Cria cópias dos recursos disponíveis e do número de VMs alojadas
     * em cada servidor. A validação pode, assim, simular a reserva sem
     * alterar os recursos reais do centro de dados.
     */
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

        /* Os identificadores dos servidores começam em 1, enquanto
         * os índices dos vetores começam em 0. */
		if (item->server_id == 0 || item->server_id > dc->num_servers) {
			fprintf(stderr, "Invalid server in reservation.\n");
			return 1;
		}

		for (size_t j = 0; j < item->amount; j++) {

            /* Impede que a reserva ultrapasse o número máximo de VMs. */
			if(vms_count >= MAX_RESERVATION_VMS){
				fprintf(stderr, "Reached max of VMS for a reservation.\n");
				return 1;
			}
			
            /* Verifica o limite de VMs alojadas no servidor escolhido. */            
			if (hosted_count[item->server_id - 1] >= MAX_HOSTED_VMS) {
                fprintf(stderr,"Server \"%zu\" reached the maximum number of hosted VMs.\n",item->server_id);
                return 1;
    	    }

            /* Confirma que o servidor tem recursos suficientes para
             * alojar mais uma VM deste tipo. */
			if (!resources_can_fit(available[item->server_id-1], vm_type->required)) {
				fprintf(stderr, "Server \"%zu\" does not have enough resources.\n", item->server_id);
				return 1;
			}

            /*
             * Atualiza apenas as cópias temporárias, para que as VMs
             * seguintes também sejam validadas tendo em conta os
             * recursos que esta VM hipoteticamente ocuparia.
             */
			resources_sub(&available[item->server_id-1], vm_type->required);
			hosted_count[item->server_id-1]++;
			vms_count++; // One VM "reserved"
		}

        /* Guarda o ponteiro para o tipo de VM validado, permitindo
         * utilizá-lo posteriormente durante a criação das VMs. */
		item->vm_type = vm_type;
	}

	return 0;
}

/*
 * Reverte as alterações realizadas durante a criação de uma reserva.
 * Apenas remove as VMs acrescentadas depois de initial_vms, permitindo
 * recuperar o estado anterior caso a operação falhe a meio.
 */
static void reservation_rollback(Reservation *reservation, size_t initial_vms){
	while (reservation->num_vms > initial_vms) {
		VM *vm = reservation->vms[reservation->num_vms - 1];

		Server *server = vm->server;

        /* Devolve ao servidor os recursos ocupados pela VM. */
		resources_add(&server->available, vm->type->required);

        /* A VM foi acrescentada no fim da lista do servidor.
         * Remove essa última entrada e atualiza o número de VMs. */
        server->num_hosted_vms--;

		server->hosted_vms[server->num_hosted_vms] = NULL;

        /* Remove também a VM da reserva. */
		reservation->num_vms--;
		reservation->vms[reservation->num_vms] = NULL;
		free(vm);
	}
}

/* Copia o conteúdo de um ficheiro para outro.
 * Devolve 0 em caso de sucesso e -1 se ocorrer um erro. */
static int copy_file(const char *src_path, const char *dst_path) {
    int src_fd = open(src_path, O_RDONLY);
    if (src_fd < 0) {
        return -1;
    }
    
    /* Cria o ficheiro de destino ou substitui o seu conteúdo
     * caso já exista. */
    int dst_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (dst_fd < 0) {
        close(src_fd);
        return -1;
    }

    char buffer[4096];
    ssize_t bytes_read;
    int result = 0;

    /* Lê o ficheiro em blocos para não carregar todo o conteúdo na memória. */
    while ((bytes_read = read(src_fd, buffer, sizeof(buffer))) > 0) {
        ssize_t total_written = 0;

        /* write() pode escrever menos bytes do que os pedidos.
         * O ciclo garante que todos os bytes lidos são escritos. */
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

    /* Regista também eventuais erros ao fechar os ficheiros. */
    if (close(src_fd) < 0) {
        result = -1;
    }

    if (close(dst_fd) < 0) {
        result = -1;
    }

    return result;
}

/*
 * Copia recursivamente o conteúdo de uma diretoria.
 * As subdiretorias são processadas pela mesma função e os ficheiros
 * regulares são copiados com copy_file().
 */
static int copy_directory_recursive(const char *src, const char *dst) {
    DIR *dir = opendir(src);
    if (dir == NULL) {
        return -1;
    }

    struct stat st;

    /* Garante que o destino existe e é uma diretoria. */    
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

        /* Ignora as referências à própria diretoria e à diretoria anterior. */
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char src_path[MAX_PATH_SIZE];
        char dst_path[MAX_PATH_SIZE];

        /* Constrói os caminhos completos de origem e destino.
         * snprintf() permite verificar se os caminhos cabem nos vetores. */
        int src_len = snprintf(src_path, sizeof(src_path), "%s/%s", src, entry->d_name);
        int dst_len = snprintf(dst_path, sizeof(dst_path), "%s/%s", dst, entry->d_name);

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

        /*
         * Escolhe a operação de cópia segundo o tipo do caminho:
         * diretoria: nova chamada recursiva;
         * ficheiro regular: cópia do conteúdo.
         */
        if (S_ISDIR(entry_stat.st_mode)) {
            copy_result = copy_directory_recursive(src_path, dst_path);
        } else if (S_ISREG(entry_stat.st_mode)) {
            copy_result = copy_file(src_path, dst_path);
        } else {

            /* Ignora outros tipos de entrada. */
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

/*
 * Prepara o sistema de ficheiros de uma VM:
 *   1. Garante que a diretoria base existe.
 *   2. Garante que existe uma diretoria para a reserva.
 *   3. Cria a diretoria específica da VM.
 *   4. Copia para essa diretoria os ficheiros de entrada do tipo de VM.
 */
static int setup_vm_filesystem(
    const char *reservation_id,
    const char *vm_id,
    const char *input_folder
) {
    const char *base_path = "/tmp/CloudIST-ist187532";
    char reservation_path[MAX_PATH_SIZE];
    char vm_path[MAX_PATH_SIZE];

    struct stat st;

    /* Cria a diretoria base, caso ainda não exista. */
    if (stat(base_path, &st) != 0) {
        if (mkdir(base_path, 0777) != 0) {
            return -1;
        }
    } else if (!S_ISDIR(st.st_mode)) {
        return -1;
    }

    /* Constrói o caminho da diretoria da reserva. */
    int reservation_len = snprintf(reservation_path,sizeof(reservation_path),"%s/%s",base_path,reservation_id);

    if (reservation_len < 0 || (size_t)reservation_len >= sizeof(reservation_path)) {
        return -1;
    }

    /* Cria a diretoria da reserva, se necessário. */
    if (stat(reservation_path, &st) != 0) {
        if (mkdir(reservation_path, 0777) != 0) {
            return -1;
        }
    } else if (!S_ISDIR(st.st_mode)) {
        return -1;
    }

    /* Constrói o caminho exclusivo desta VM. */
    int vm_len = snprintf(vm_path,sizeof(vm_path),"%s/%s",reservation_path,vm_id);

    if (vm_len < 0 || (size_t)vm_len >= sizeof(vm_path)) {
        return -1;
    }

    /* A diretoria da VM deve ser criada antes de copiar os ficheiros. */
    if (mkdir(vm_path, 0777) != 0) {
        return -1;
    }

    return copy_directory_recursive(input_folder, vm_path);
}

/* Guarda o número inicial de VMs para que seja possível reverter
 * apenas as VMs criadas por esta operação, caso ocorra um erro. */
int reservation_commit(DataCenter *dc, Reservation *reservation){
	size_t initial_vms = reservation->num_vms;

	for (size_t i = 0; i < reservation->items_count; i++) {
		ReservationItem *item = &reservation->items[i];
		Server *server = &dc->servers[item->server_id - 1];

		for (size_t j = 0; j < item->amount; j++) {

            /* Reserva memória para representar a nova VM. */            
			VM *new_vm = calloc(1, sizeof(VM));
			if (new_vm == NULL) {
				fprintf(stderr, "Failed to allocate VM.\n");
				reservation_rollback(reservation, initial_vms);
				return 1;
			}

            /* Gera um identificador com o formato
             * <identificador_da_reserva>_<número_da_VM>. */
			snprintf(new_vm->id,MAX_VM_ID_STRING,"%s_%zu",reservation->id,reservation->num_vms + 1);

            /* Associa a VM ao tipo, servidor e estado inicial. */			
			new_vm->type = item->vm_type;
			new_vm->server = server;
			new_vm->state = VM_STATE_RESERVED;

            /*
             * Prepara a diretoria da VM e copia os ficheiros de entrada.
             * Se a operação falhar, a VM ainda não foi registada nas
             * listas nem consumiu recursos do servidor.
             */
			if (setup_vm_filesystem(reservation->id, new_vm->id, item->vm_type->input_folder) != 0) {
    			fprintf(stderr, "Failed to set up filesystem for VM %s.\n", new_vm->id);
    			free(new_vm);
    			reservation_rollback(reservation, initial_vms);
    			return 1;
			}

            /* Após preparar o sistema de ficheiros, contabiliza os
             * recursos consumidos e regista a VM no servidor e na reserva. */
			resources_sub(&server->available, item->vm_type->required);

			server->hosted_vms[server->num_hosted_vms++] = new_vm;
			reservation->vms[reservation->num_vms++] = new_vm;
		}
	}

    /* A reserva só fica pendente depois de todas as VMs serem criadas. */
	reservation->state = RES_STATE_PENDING;

	return 0;
}

/*
 * Procura uma reserva pelo identificador e confirma que está pendente.
 * Uma reserva inexistente ou num estado diferente não pode ser devolvida
 * para operações que exijam uma reserva pendente.
 */
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

/*
 * Remove recursivamente um ficheiro ou uma diretoria e o seu conteúdo.
 * Devolve 0 se a remoção for concluída ou se o caminho já não existir;
 * devolve -1 quando ocorre um erro.
 */
int remove_path_recursive(const char *path) {
    struct stat st;

    /*
     * lstat() permite reconhecer ligações simbólicas
     * sem seguir o seu destino.
     */
    if (lstat(path, &st) == -1) {
        if (errno == ENOENT) {
            /* O caminho já não existe. */
            return 0;
        }

        return -1;
    }

    /* Ficheiros e ligações simbólicas podem ser removidos diretamente.
     * As diretorias exigem a remoção prévia do seu conteúdo. */
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
        
        /* Reinicializa errno antes de readdir() para distinguir
         * o fim normal da leitura de um erro. */        
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

        /* Constrói o caminho da entrada que será removida. */
        int n = snprintf(child_path,sizeof(child_path),"%s/%s",path,entry->d_name);

        if (n < 0 || (size_t)n >= sizeof(child_path)) {
            result = -1;
            continue;
        }

        /* Remove cada entrada, incluindo o conteúdo das subdiretorias. */        
        if (remove_path_recursive(child_path) != 0) {
            result = -1;
        }
    }

    if (closedir(dir) != 0) {
        result = -1;
    }

    /* Só remove a própria diretoria depois de processar as entradas
     * que contém. rmdir() falha se ainda existirem entradas no seu interior. */
    if (rmdir(path) != 0) {
        result = -1;
    }

    return result;
}

void reservation_destroy(DataCenter *dc, Reservation *reservation) {
    /* Liberta todas as VMs associadas à reserva. */
	for (size_t i = 0; i < reservation->num_vms; i++) {
		VM *vm = reservation->vms[i];
		Server *server = vm->server;

        /* Devolve ao servidor os recursos utilizados pela VM. */
		resources_add(&server->available, vm->type->required);

        /* Procura a VM na lista do servidor e remove-a.
         * As entradas seguintes são deslocadas para manter a lista compacta.*/		
        for (size_t j = 0; j < server->num_hosted_vms; j++) {
			if (server->hosted_vms[j] == vm) {
				memmove(&server->hosted_vms[j],&server->hosted_vms[j + 1],(server->num_hosted_vms - j - 1) * sizeof(VM *));
				server->num_hosted_vms--;
				break;
			}
		}

		free(vm);
		reservation->vms[i] = NULL;
	}

	reservation->num_vms = 0;

    /*
     * Identifica a posição da reserva no vetor do centro de dados.
     * A comparação dos endereços permite localizar a estrutura exata,
     * em vez de procurar apenas pelo identificador.
     */	
    size_t idx;
	for (idx = 0; idx < dc->num_reservations; idx++) {
			if (&dc->reservations[idx] == reservation)
				break;
	}

    /* A reserva não está no vetor; não há registo para remover. */
	if (idx == dc->num_reservations)
		return;

    /* Constrói o caminho da diretoria que pertence à reserva. */
    char reservation_path[MAX_PATH_SIZE];

    int n = snprintf(reservation_path,sizeof(reservation_path),"/tmp/CloudIST-ist187532/%s",reservation->id);

    if (n < 0 || (size_t)n >= sizeof(reservation_path)) {
        fprintf(stderr, "Reservation path too long.\n");
        return;
    }

    /* Remove a diretoria da reserva e todo o seu conteúdo.
     * A remoção é independente da libertação das estruturas em memória. */
    if (remove_path_recursive(reservation_path) != 0) {
        
        /* Regista a falha, mas continua a remoção do registo da reserva
         * do centro de dados.*/
        perror("Failed to remove reservation directory");
    }

    /* Remove a reserva do vetor deslocando as reservas seguintes.
     * O número de reservas é atualizado depois do deslocamento. */	
    memmove(&dc->reservations[idx],&dc->reservations[idx + 1],(dc->num_reservations - idx - 1) * sizeof(Reservation));
	dc->num_reservations--;
}

int remove_completed_reservation(DataCenter *dc, Reservation *res) {
    
    /* Só aceita uma reserva válida sem VMs associadas.
     * Os argumentos e o estado da reserva são verificados antes da remoção. */
    if (dc == NULL || res == NULL || res->num_vms != 0) {
        fprintf(stderr, "Invalid completed reservation.\n");
        return 1;
    }

    /* Procura a posição exata da reserva no vetor do centro de dados. */
    size_t idx = 0;

    while (idx < dc->num_reservations && &dc->reservations[idx] != res) {
        idx++;
    }

    if (idx == dc->num_reservations) {
        fprintf(stderr, "Reservation not found.\n");
        return 1;
    }

    /* Constrói o caminho da diretoria que pertence à reserva concluída. */
    char reservation_path[MAX_PATH_SIZE];

    int n = snprintf(reservation_path,sizeof(reservation_path),"/tmp/CloudIST-ist187532/%s",res->id);

    if (n < 0 || (size_t)n >= sizeof(reservation_path)) {
        fprintf(stderr, "Reservation path too long.\n");
        return 1;
    }

    /* Se a remoção da diretoria falhar, não remove o registo da reserva.
     * Assim, o chamador recebe indicação de que a operação não terminou. */
    if (remove_path_recursive(reservation_path) != 0) {
        perror("Failed to remove reservation directory");
        return 1;
    }

    /* Remove a reserva do vetor, deslocando as entradas seguintes. */
    size_t remaining = dc->num_reservations - idx - 1;

    if (remaining > 0) {
        memmove(&dc->reservations[idx],&dc->reservations[idx + 1],remaining * sizeof(dc->reservations[0]));
    }

    dc->num_reservations--;

    /* Limpa a posição final, que deixou de pertencer ao vetor em utilização. */
    memset(&dc->reservations[dc->num_reservations],0,sizeof(dc->reservations[0]));
    return 0;
}

/* Converte o estado de uma VM numa cadeia de caracteres.
 * UNKNOWN representa um valor que não corresponde a um estado conhecido. */
const char *vm_state_to_string(VMState state) {
    switch (state) {
        case VM_STATE_RESERVED:   return "RESERVED";
        case VM_STATE_RUNNING:    return "RUNNING";
        case VM_STATE_TERMINATED: return "TERMINATED";
        default:                  return "UNKNOWN";
    }
}

/* Converte o estado de uma reserva numa cadeia de caracteres. */
const char *res_state_to_string(ReservationState state) {
    switch (state) {
			case RES_STATE_PENDING:  return "PENDING";
			case RES_STATE_RUNNING:  return "RUNNING";
			case RES_STATE_FINISHED: return "FINISHED";
			default:                 return "UNKNOWN";
    }
}