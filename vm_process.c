#include "vm_process.h"
#include "datacenter_utils.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <string.h>

void spawn_vm_child(VM *vm, size_t total_physical_cpus, size_t local_cores,const char *executable_path) {
    /*
     * 1. Limitar o tamanho máximo de cada ficheiro (disco).
     */
    struct rlimit rl_disk;

    rl_disk.rlim_cur = (rlim_t)vm->type->required.disk * 1024ULL * 1024ULL * 1024ULL;
    rl_disk.rlim_max = rl_disk.rlim_cur;

    if (setrlimit(RLIMIT_FSIZE, &rl_disk) == -1) {
        perror("setrlimit RLIMIT_FSIZE");
        exit(1);
    }

    /*
     * 2. Limitar o espaço de endereçamento virtual (RAM).
     */
    struct rlimit rl_ram;

    rl_ram.rlim_cur = (rlim_t)vm->type->required.ram * 1024ULL * 1024ULL * 1024ULL;
    rl_ram.rlim_max = rl_ram.rlim_cur;

    if (setrlimit(RLIMIT_AS, &rl_ram) == -1) {
        perror("setrlimit RLIMIT_AS");
        exit(1);
    }

    /*
     * 3. Validar o total de cores físicos.
     */
    if (total_physical_cpus == 0 || local_cores == 0) {
        write(STDERR_FILENO,"Invalid CPU configuration.\n",sizeof("Invalid CPU configuration.\n") - 1);
        exit(1);
    }

    long double percentage = 100.0L * (long double)vm->type->required.cpu * (long double)local_cores / (long double)total_physical_cpus;

    if (percentage > (long double)SIZE_MAX - 0.5L) {
        write(STDERR_FILENO,"CPU limit is too large.\n",sizeof("CPU limit is too large.\n") - 1);
        exit(1);
    }

    size_t cpu_limit = (size_t)(percentage + 0.5L);

    if (cpu_limit < 1) {
        cpu_limit = 1;
    }

    char cpu_limit_str[32];

    int n = snprintf(cpu_limit_str,sizeof(cpu_limit_str),"%zu",cpu_limit);

    if (n < 0 || (size_t)n >= sizeof(cpu_limit_str)) {
        write(STDERR_FILENO,"Failed to format CPU limit.\n",sizeof("Failed to format CPU limit.\n") - 1);
        exit(1);
    }

    /*
     * 5. Executar a aplicação com cpulimit.
     */
    execlp("cpulimit", "cpulimit", "-q", "-f", "-l",cpu_limit_str, "--", executable_path, (char *)NULL);    /*
     * Só chegamos aqui se execlp() falhar.
     */
    perror("execlp cpulimit");
    exit(127);
}

void rollback_spawned_vms(VM **started, size_t started_count) {
    /*
     * Terminar todos os processos criados durante
     * esta tentativa de arranque.
     */
    for (size_t i = 0; i < started_count; i++) {
        if (started[i]->pid > 0) {
            (void)kill(started[i]->pid, SIGKILL);
        }
    }

    /*
     * Recolher os processos e restaurar os estados.
     */
    for (size_t i = 0; i < started_count; i++) {
        VM *vm = started[i];

        if (vm->pid > 0) {
            pid_t result;

            do {
                result = waitpid(vm->pid, NULL, 0);
            } while (result == -1 && errno == EINTR);
        }

        vm->pid = 0;
        vm->state = VM_STATE_RESERVED;
    }
}


int spawn_all_vms(Reservation *res, size_t total_physical_cpus,size_t local_cores) {
    if (res == NULL || total_physical_cpus == 0) {
        fprintf(stderr, "Invalid reservation or CPU count.\n");
        return 1;
    }

    if (res->num_vms == 0) {
        fprintf(stderr, "Reservation contains no VMs.\n");
        return 1;
    }

    /*
     * Guardar as VMs iniciadas nesta tentativa,
     * para as podermos terminar se algum fork falhar.
     */
    VM **started = calloc(res->num_vms, sizeof(*started));

    if (started == NULL) {
        perror("calloc");
        return 1;
    }

    size_t started_count = 0;

    for (size_t i = 0; i < res->num_vms; i++) {
        VM *vm = res->vms[i];

        /* Só arrancar VMs reservadas. */
        if (vm->state != VM_STATE_RESERVED) {
            continue;
        }

        char executable_path[MAX_PATH_SIZE];

        if (vm->type->exec_path[0] == '/') {
            int n = snprintf(executable_path,sizeof(executable_path),"%s",vm->type->exec_path);

            if (n < 0 || (size_t)n >= sizeof(executable_path)) {
                fprintf(stderr, "Executable path too long.\n");
                free(started);
                return 1;
            }
        } 
        else {
            char cwd[MAX_PATH_SIZE];
            if (getcwd(cwd, sizeof(cwd)) == NULL) {
                perror("getcwd");
                free(started);
                return 1;
            }

            int n = snprintf(executable_path,sizeof(executable_path),"%s/%s",cwd,vm->type->exec_path);

        if (n < 0 || (size_t)n >= sizeof(executable_path)) {
            fprintf(stderr, "Executable path too long.\n");
            free(started);
            return 1;
        }
    }

        pid_t pid = fork();

        if (pid == -1) {
            perror("fork");

            rollback_spawned_vms(started, started_count);

            res->state = RES_STATE_PENDING;

            free(started);
            return 1;
        }

        if (pid == 0) {
            /* PROCESSO FILHO */

            char vm_path[MAX_PATH_SIZE];

            int n = snprintf(
                vm_path,
                sizeof(vm_path),
                "/tmp/CloudIST-ist187532/%s/%s",
                res->id,
                vm->id
            );

            if (n < 0 || (size_t)n >= sizeof(vm_path)) {
                write(STDERR_FILENO,"VM path too long.\n", sizeof("VM path too long.\n") - 1);
                exit(1);
            }

            /* Entrar na diretoria da VM. */
            if (chdir(vm_path) == -1) {
                perror("chdir VM");
                exit(1);
            }

            /* Abrir/criar o ficheiro de saída. */
            int fd_out = open(
                "stdout",
                O_WRONLY | O_CREAT | O_TRUNC,
                0666
            );

            if (fd_out == -1) {
                perror("open stdout");
                exit(1);
            }

            /* Redirecionar stdout para o ficheiro. */
            if (fd_out != STDOUT_FILENO) {
                if (dup2(fd_out, STDOUT_FILENO) == -1) {
                    perror("dup2 stdout");
                    close(fd_out);
                    exit(1);
                }

                if (close(fd_out) == -1) {
                    perror("close stdout");
                    exit(1);
                }
            }

            /* Aplicar limites e executar a aplicação. */
            spawn_vm_child(vm, total_physical_cpus, local_cores,executable_path);

            /* Segurança: a função não deverá regressar. */
            exit(127);
        }

        /* PROCESSO PAI */

        vm->pid = pid;
        vm->state = VM_STATE_RUNNING;

        started[started_count++] = vm;
    }

    free(started);

    /*
     * A reserva só passa para RUNNING depois de
     * concluída a tentativa de criar os processos.
     */
    res->state = RES_STATE_RUNNING;

    return 0;
}


int reap_finished_vms(DataCenter *dc) {
    if (dc == NULL) {
        fprintf(stderr, "Invalid DataCenter.\n");
        return 1;
    }

    int errors = 0;
    size_t i = 0;

    /*
     * Usamos while porque a remoção de uma reserva
     * desloca as reservas seguintes no array.
     */
    while (i < dc->num_reservations) {
        Reservation *res = &dc->reservations[i];
        size_t j = 0;

        while (j < res->num_vms) {
            VM *vm = res->vms[j];

            if (vm == NULL) {
                fprintf(stderr,
                        "NULL VM in reservation %s.\n",
                        res->id);
                errors = 1;
                j++;
                continue;
            }

            /*
             * Tentar novamente a destruição de uma VM
             * que já foi confirmada como terminada.
             */
            if (vm->state == VM_STATE_TERMINATED &&
                vm->pid == 0) {
                if (vm_destroy(res, vm) != 0) {
                    errors = 1;
                    j++;
                }

                /*
                 * Se a destruição teve sucesso, a próxima VM
                 * ocupa a mesma posição j.
                 */
                continue;
            }

            if (vm->state != VM_STATE_RUNNING) {
                j++;
                continue;
            }

            if (vm->pid <= 0) {
                fprintf(stderr,
                        "Invalid PID for VM %s.\n",
                        vm->id);
                errors = 1;
                j++;
                continue;
            }

            int status;
            pid_t result;

            do {
                result = waitpid(vm->pid, &status, WNOHANG);
            } while (result == -1 && errno == EINTR);

            if (result == 0) {
                /* O processo ainda está ativo. */
                j++;
                continue;
            }

            if (result == vm->pid) {
                /*
                 * O processo terminou e foi recolhido.
                 */
                vm->state = VM_STATE_TERMINATED;
                vm->pid = 0;

                if (WIFEXITED(status)) {
                    int exit_status = WEXITSTATUS(status);

                    if (exit_status != 0) {
                        fprintf(stderr,
                                "VM %s exited with status %d.\n",
                                vm->id,
                                exit_status);
                    }
                } else if (WIFSIGNALED(status)) {
                    fprintf(stderr,
                            "VM %s terminated by signal %d.\n",
                            vm->id,
                            WTERMSIG(status));
                }

                if (vm_destroy(res, vm) != 0) {
                    errors = 1;
                    j++;
                }

                /*
                 * Se vm_destroy() teve sucesso, não incrementamos j,
                 * porque o array de VMs foi deslocado.
                 */
                continue;
            }

            if (result == -1) {
                if (errno == ECHILD) {
                /*
                 * O processo já não pode ser recolhido.
                 * Na implementação atual, assumimos que terminou
                 * e que não existe outro código a recolher os filhos.
                 */
                    fprintf(stderr, "Warning: VM %s is no longer waitable.\n", vm->id);
                    vm->state = VM_STATE_TERMINATED;
                    vm->pid = 0;

                    if (vm_destroy(res, vm) != 0) {
                        errors = 1;
                        j++;
                    }
                continue;
                }
                perror("waitpid");
                errors = 1;
                j++;
            }
        }

        /*
         * Se todas as VMs terminaram, remover a reserva.
         * Também tentamos novamente uma limpeza que tenha
         * falhado numa chamada anterior.
         */
        if (res->num_vms == 0 &&
            (res->state == RES_STATE_RUNNING ||
             res->state == RES_STATE_FINISHED)) {

            res->state = RES_STATE_FINISHED;

            if (remove_completed_reservation(dc, res) != 0) {
                errors = 1;
                i++;
            }

            /*
             * Se a reserva foi removida, outra reserva
             * ocupa agora a posição i. Não incrementamos i.
             */
            else {
                continue;
            }
        } else {
            i++;
        }
    }

    return errors;
}

int vm_destroy(Reservation *reservation, VM *vm) {
    if (reservation == NULL || vm == NULL ||
        vm->server == NULL || vm->type == NULL) {
        fprintf(stderr, "Invalid VM or reservation.\n");
        return 1;
    }

    /*
     * Só podemos destruir uma VM depois de confirmar
     * que o processo terminou e foi recolhido com waitpid().
     */
    if (vm->state != VM_STATE_TERMINATED || vm->pid != 0) {
        fprintf(stderr,"VM %s has not been confirmed terminated.\n",vm->id);
        return 1;
    }

    Server *server = vm->server;

    /*
     * Procurar a VM na lista da reserva.
     */
    size_t reservation_index = reservation->num_vms;

    for (size_t i = 0; i < reservation->num_vms; i++) {
        if (reservation->vms[i] == vm) {
            reservation_index = i;
            break;
        }
    }

    if (reservation_index == reservation->num_vms) {
        fprintf(stderr,
                "VM %s not found in reservation.\n",
                vm->id);
        return 1;
    }

    /*
     * Procurar a VM na lista do servidor.
     */
    size_t server_index = server->num_hosted_vms;

    for (size_t i = 0; i < server->num_hosted_vms; i++) {
        if (server->hosted_vms[i] == vm) {
            server_index = i;
            break;
        }
    }

    if (server_index == server->num_hosted_vms) {
        fprintf(stderr,
                "VM %s not found in server hosted list.\n",
                vm->id);
        return 1;
    }

    /*
     * 1. Devolver os recursos ao servidor.
     */
    resources_add(&server->available, vm->type->required);

    /*
     * 2. Remover a VM da lista do servidor.
     */
    memmove(&server->hosted_vms[server_index],
            &server->hosted_vms[server_index + 1],
            (server->num_hosted_vms - server_index - 1)
                * sizeof(server->hosted_vms[0]));

    server->num_hosted_vms--;

    server->hosted_vms[server->num_hosted_vms] = NULL;

    /*
     * 3. Remover a VM da lista da reserva.
     */
    memmove(&reservation->vms[reservation_index],
            &reservation->vms[reservation_index + 1],
            (reservation->num_vms - reservation_index - 1)
                * sizeof(reservation->vms[0]));

    reservation->num_vms--;

    reservation->vms[reservation->num_vms] = NULL;

    /*
     * Remover a diretoria da VM e o respetivo conteúdo.
     * Se falhar, a limpeza da diretoria da reserva
     * tentará novamente mais tarde.
     */
    
    char vm_path[MAX_PATH_SIZE];

    int n = snprintf(vm_path,
                     sizeof(vm_path),
                     "/tmp/CloudIST-ist187532/%s/%s",
                     reservation->id,
                     vm->id);

    if (n < 0 || (size_t)n >= sizeof(vm_path)) {
        fprintf(stderr,
                "VM path too long for %s.\n",
                vm->id);
    } else if (remove_path_recursive(vm_path) != 0) {
        perror("Failed to remove VM directory");
    }
    
    /*
     * 4. Libertar a estrutura da VM.
     */
    free(vm);

    return 0;
}