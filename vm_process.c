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

/*
 * Executa a aplicação associada a uma VM no processo-filho.
 *
 * Antes de iniciar a aplicação, define os limites de disco e memória
 * e calcula o limite de CPU. Por fim, substitui o processo atual pelo
 * cpulimit, que executa o programa com o limite calculado.
 *
 * Esta função só termina normalmente se ocorrer um erro, uma vez que
 * execlp() substitui a imagem do processo quando tem sucesso.
 */
void spawn_vm_child(VM *vm, size_t total_physical_cpus, size_t local_cores, const char *executable_path) {
    
    /*
     * Limita o tamanho máximo de cada ficheiro criado pela aplicação.
     * O limite é expresso em bytes e corresponde ao espaço de disco
     * atribuído ao tipo de VM.
     */
    struct rlimit rl_disk;

    rl_disk.rlim_cur = (rlim_t)vm->type->required.disk * 1024ULL * 1024ULL * 1024ULL;
    rl_disk.rlim_max = rl_disk.rlim_cur;

    if (setrlimit(RLIMIT_FSIZE, &rl_disk) == -1) {
        perror("setrlimit RLIMIT_FSIZE");
        exit(1);
    }

    /*
     * Limita o espaço de endereçamento virtual do processo, utilizando
     * a quantidade de RAM atribuída ao tipo de VM, convertida em bytes.
     */
    struct rlimit rl_ram;

    rl_ram.rlim_cur = (rlim_t)vm->type->required.ram * 1024ULL * 1024ULL * 1024ULL;
    rl_ram.rlim_max = rl_ram.rlim_cur;

    if (setrlimit(RLIMIT_AS, &rl_ram) == -1) {
        perror("setrlimit RLIMIT_AS");
        exit(1);
    }

    /*
     * Confirma que os valores utilizados no cálculo do limite de CPU
     * são válidos. Ambos têm de ser diferentes de zero.
     */
    if (total_physical_cpus == 0 || local_cores == 0) {
        write(STDERR_FILENO, "Invalid CPU configuration.\n", sizeof("Invalid CPU configuration.\n") - 1);
        exit(1);
    }

    /*
     * Calcula a percentagem de CPU permitida para a VM, tendo em conta
     * os vCPUs pedidos, os núcleos locais disponíveis e o total de CPUs
     * físicos utilizado como referência.
     */
    long double percentage = 100.0L * (long double)vm->type->required.cpu * (long double)local_cores / (long double)total_physical_cpus;

    /* Evita converter um valor demasiado grande para size_t. */
    if (percentage > (long double)SIZE_MAX - 0.5L) {
        write(STDERR_FILENO, "CPU limit is too large.\n", sizeof("CPU limit is too large.\n") - 1);
        exit(1);
    }

    /* Arredonda a percentagem para o inteiro mais próximo. */
    size_t cpu_limit = (size_t)(percentage + 0.5L);

    /* Garante um limite mínimo de 1%. */
    if (cpu_limit < 1) {
        cpu_limit = 1;
    }

    /* O limite de CPU é passado ao cpulimit como uma cadeia de caracteres. */
    char cpu_limit_str[32];

    int n = snprintf(cpu_limit_str, sizeof(cpu_limit_str), "%zu", cpu_limit);

    if (n < 0 || (size_t)n >= sizeof(cpu_limit_str)) {
        write(STDERR_FILENO, "Failed to format CPU limit.\n", sizeof("Failed to format CPU limit.\n") - 1);
        exit(1);
    }

    /*
     * Executa a aplicação através do cpulimit, que controla a utilização
     * de CPU do processo. A opção -f mantém o cpulimit em primeiro plano
     * e -q reduz a saída de mensagens.
     *
     * Se execlp() tiver sucesso, o código seguinte não será executado.
     */
    execlp("cpulimit", "cpulimit", "-q", "-f", "-l", cpu_limit_str, "--", executable_path, (char *)NULL);

    /* Só se chega aqui se não for possível executar o cpulimit. */
    perror("execlp cpulimit");
    exit(127);
}

/*
 * Reverte o arranque parcial de um conjunto de VMs.
 *
 * É utilizada quando não é possível criar todos os processos de uma
 * reserva. Primeiro termina os processos já iniciados e depois espera
 * por eles, para evitar deixar processos-filhos por recolher.
 *
 * As VMs afetadas regressam ao estado reservado.
 */
void rollback_spawned_vms(VM **started, size_t started_count) {
    
    /* Envia SIGKILL a todos os processos iniciados nesta tentativa.
     * O sinal termina os processos sem permitir que continuem a executar. */
    for (size_t i = 0; i < started_count; i++) {
        if (started[i]->pid > 0) {
            (void)kill(started[i]->pid, SIGKILL);
        }
    }

    /* Recolhe os processos-filhos com waitpid().
     * Se a espera for interrompida por um sinal, tenta novamente. */
    for (size_t i = 0; i < started_count; i++) {
        VM *vm = started[i];

        if (vm->pid > 0) {
            pid_t result;

            do {
                result = waitpid(vm->pid, NULL, 0);
            } while (result == -1 && errno == EINTR);
        }

        /* Repõe os campos da VM para permitir uma tentativa posterior. */
        vm->pid = 0;
        vm->state = VM_STATE_RESERVED;
    }
}

/*
 * Inicia os processos correspondentes às VMs de uma reserva.
 *
 * Para cada VM reservada, cria um processo-filho, prepara o ambiente
 * de execução e inicia a aplicação. No processo-pai, guarda o PID
 * e atualiza o estado da VM.
 *
 * Se uma chamada a fork() falhar, termina e recolhe os processos
 * já iniciados nesta tentativa.
 */
int spawn_all_vms(Reservation *res, size_t total_physical_cpus, size_t local_cores) {
    if (res == NULL || total_physical_cpus == 0) {
        fprintf(stderr, "Invalid reservation or CPU count.\n");
        return 1;
    }

    /* Não é possível iniciar uma reserva sem VMs. */
    if (res->num_vms == 0) {
        fprintf(stderr, "Reservation contains no VMs.\n");
        return 1;
    }

    /* Guarda as referências às VMs iniciadas com sucesso.
     * Este vetor permite reverter o arranque se um fork() falhar. */
    VM **started = calloc(res->num_vms, sizeof(*started));

    if (started == NULL) {
        perror("calloc");
        return 1;
    }

    size_t started_count = 0;

    for (size_t i = 0; i < res->num_vms; i++) {
        VM *vm = res->vms[i];

        /* Ignora VMs que não estejam no estado reservado. */
        if (vm->state != VM_STATE_RESERVED) {
            continue;
        }

        char executable_path[MAX_PATH_SIZE];

        /*
         * Se o caminho do executável já for absoluto, copia-o diretamente.
         * Assim, o caminho continua válido depois de o processo-filho mudar
         * de diretoria para a pasta de trabalho da VM.
         */
        if (vm->type->exec_path[0] == '/') {
            int n = snprintf(executable_path, sizeof(executable_path), "%s", vm->type->exec_path);

            if (n < 0 || (size_t)n >= sizeof(executable_path)) {
                fprintf(stderr, "Executable path too long.\n");
                free(started);
                return 1;
            }
        } else {
            
            /* Se o caminho for relativo, transforma-o num caminho absoluto
             * com base na diretoria de trabalho atual do processo-pai. */
            char cwd[MAX_PATH_SIZE];

            if (getcwd(cwd, sizeof(cwd)) == NULL) {
                perror("getcwd");
                free(started);
                return 1;
            }

            int n = snprintf(executable_path, sizeof(executable_path), "%s/%s", cwd, vm->type->exec_path);

            if (n < 0 || (size_t)n >= sizeof(executable_path)) {
                fprintf(stderr, "Executable path too long.\n");
                free(started);
                return 1;
            }
        }

        /* Cria um novo processo. O valor de retorno permite distinguir
         * o processo-pai, o processo-filho e uma falha na criação. */
        pid_t pid = fork();

        if (pid == -1) {
            perror("fork");

            /* Termina os processos criados antes desta falha. */
            rollback_spawned_vms(started, started_count);

            res->state = RES_STATE_PENDING;

            free(started);
            return 1;
        }

        if (pid == 0) {
            /* PROCESSO-FILHO: prepara o ambiente e executa a VM. */

            char vm_path[MAX_PATH_SIZE];

            /* Cada VM utiliza a sua própria diretoria para os ficheiros
             * de entrada e para o ficheiro de saída stdout. */
            int n = snprintf(vm_path,sizeof(vm_path),"/tmp/CloudIST-ist187532/%s/%s",res->id,vm->id);

            if (n < 0 || (size_t)n >= sizeof(vm_path)) {
                write(STDERR_FILENO, "VM path too long.\n", sizeof("VM path too long.\n") - 1);
                exit(1);
            }

            /* Define a diretoria da VM como diretoria de trabalho. */
            if (chdir(vm_path) == -1) {
                perror("chdir VM");
                exit(1);
            }

            /* Cria ou substitui o ficheiro stdout dentro da diretoria
             * da VM. A saída padrão da aplicação será guardada aí. */
            int fd_out = open(
                "stdout",
                O_WRONLY | O_CREAT | O_TRUNC,
                0666
            );

            if (fd_out == -1) {
                perror("open stdout");
                exit(1);
            }

            /* Redireciona a saída padrão para o ficheiro.
             * dup2() associa STDOUT_FILENO ao descritor fd_out. */
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

            /* Aplica os limites de recursos e substitui este processo
             * pela execução do cpulimit e da aplicação associada à VM. */
            spawn_vm_child(vm, total_physical_cpus, local_cores, executable_path);

            /* Salvaguarda: spawn_vm_child() não deverá regressar. */
            exit(127);
        }

        /* PROCESSO-PAI: regista o processo criado para esta VM. */
        vm->pid = pid;
        vm->state = VM_STATE_RUNNING;

        /* Guarda a referência para permitir reverter o arranque se necessário. */
        started[started_count++] = vm;
    }

    free(started);

    /* Atualiza o estado da reserva depois de percorrer todas as VMs.
     * A função não espera que as aplicações terminem. */
    res->state = RES_STATE_RUNNING;

    return 0;
}

/*
 * Verifica se existem processos de VMs que já terminaram e recolhe-os.
 *
 * Para cada VM em execução, utiliza waitpid() com WNOHANG, evitando
 * bloquear enquanto o processo ainda estiver ativo. Quando uma VM
 * termina, atualiza o seu estado e liberta os recursos associados.
 *
 * As VMs e reservas removidas alteram os vetores correspondentes;
 * por isso, os índices só avançam quando a entrada atual permanece.
 */
int reap_finished_vms(DataCenter *dc) {
    if (dc == NULL) {
        fprintf(stderr, "Invalid DataCenter.\n");
        return 1;
    }

    int errors = 0;
    size_t i = 0;

    /* Utiliza while porque remover uma reserva desloca as seguintes
     * posições no vetor de reservas. */
    while (i < dc->num_reservations) {
        Reservation *res = &dc->reservations[i];
        size_t j = 0;

        while (j < res->num_vms) {
            VM *vm = res->vms[j];

            /* Uma entrada nula indica um estado inconsistente do vetor. */
            if (vm == NULL) {
                fprintf(stderr,
                        "NULL VM in reservation %s.\n",
                        res->id);
                errors = 1;
                j++;
                continue;
            }

            /* Se uma tentativa anterior já confirmou o fim do processo,
             * mas não conseguiu destruir a VM, tenta novamente a limpeza. */
            if (vm->state == VM_STATE_TERMINATED &&
                vm->pid == 0) {
                if (vm_destroy(res, vm) != 0) {
                    errors = 1;
                    j++;
                }

                /* Se a destruição tiver sucesso, a VM seguinte ocupa
                 * a mesma posição; por isso, não incrementa j. */
                continue;
            }

            /* Só é necessário verificar processos no estado RUNNING. */
            if (vm->state != VM_STATE_RUNNING) {
                j++;
                continue;
            }

            /* Um processo em execução deve ter um PID válido. */
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

            /* WNOHANG permite verificar o processo sem bloquear.
             * Se waitpid() for interrompido por um sinal, repete a chamada. */
            do {
                result = waitpid(vm->pid, &status, WNOHANG);
            } while (result == -1 && errno == EINTR);

            if (result == 0) {

                /* O processo continua ativo; verifica a VM seguinte. */
                j++;
                continue;
            }

            if (result == vm->pid) {

                /* O processo terminou e foi recolhido com sucesso.
                 * O PID deixa de representar um processo ativo. */
                vm->state = VM_STATE_TERMINATED;
                vm->pid = 0;

                /* Regista um código de saída diferente de zero. */
                if (WIFEXITED(status)) {
                    int exit_status = WEXITSTATUS(status);

                    if (exit_status != 0) {
                        fprintf(stderr,
                                "VM %s exited with status %d.\n",
                                vm->id,
                                exit_status);
                    }
                } else if (WIFSIGNALED(status)) {

                    /* Informa se o processo terminou devido a um sinal. */
                    fprintf(stderr,
                            "VM %s terminated by signal %d.\n",
                            vm->id,
                            WTERMSIG(status));
                }

                /*
                 * Liberta a VM terminada. Se a destruição for bem-sucedida,
                 * o vetor é encurtado e a próxima VM passa a ocupar a
                 * posição j.
                 */
                if (vm_destroy(res, vm) != 0) {
                    errors = 1;
                    j++;
                }

                continue;
            }

            if (result == -1) {
                if (errno == ECHILD) {

                    /*
                     * O processo já não pode ser recolhido por este
                     * waitpid(). A implementação assume que terminou
                     * e que nenhum outro código recolheu o processo-filho.
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

                /* Outros erros de waitpid() são comunicados. */
                perror("waitpid");
                errors = 1;
                j++;
            }
        }

        /* Quando todas as VMs da reserva foram removidas, marca a reserva
         * como concluída e tenta remover o respetivo registo e diretoria. */
        if (res->num_vms == 0 &&
            (res->state == RES_STATE_RUNNING ||
             res->state == RES_STATE_FINISHED)) {

            res->state = RES_STATE_FINISHED;

            if (remove_completed_reservation(dc, res) != 0) {
                errors = 1;
                i++;
            } else {
                
                /* A reserva foi removida e o vetor foi deslocado.
                 * A próxima reserva já ocupa a posição i. */
                continue;
            }
        } else {
            i++;
        }
    }

    return errors;
}

/*
 * Destrói uma VM cujo processo já tenha terminado e sido recolhido.
 *
 * A função devolve os recursos ao servidor, remove a VM dos vetores
 * da reserva e do servidor, elimina a diretoria da VM e liberta a
 * estrutura da VM.
 */
int vm_destroy(Reservation *reservation, VM *vm) {

    /* Confirma que os ponteiros necessários são válidos. */
    if (reservation == NULL || vm == NULL ||
        vm->server == NULL || vm->type == NULL) {
        fprintf(stderr, "Invalid VM or reservation.\n");
        return 1;
    }

    /*
     * Não liberta uma VM cujo processo ainda possa estar em execução.
     * O estado TERMINATED e o PID igual a zero indicam que o processo
     * foi dado como terminado e recolhido.
     */
    if (vm->state != VM_STATE_TERMINATED || vm->pid != 0) {
        fprintf(stderr, "VM %s has not been confirmed terminated.\n", vm->id);
        return 1;
    }

    Server *server = vm->server;

    /* Procura a VM no vetor da reserva. */
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

    /* Procura a mesma VM no vetor de VMs alojadas pelo servidor. */
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

    /* Devolve os recursos utilizados pela VM ao conjunto de recursos
     * disponíveis no servidor. */
    resources_add(&server->available, vm->type->required);

    /* Remove a VM do vetor do servidor, deslocando as entradas seguintes
     * para a esquerda para manter as posições utilizadas consecutivas. */
    memmove(&server->hosted_vms[server_index],&server->hosted_vms[server_index + 1],(server->num_hosted_vms - server_index - 1) * sizeof(server->hosted_vms[0]));

    server->num_hosted_vms--;

    /* Limpa a posição que deixou de pertencer ao vetor em utilização. */
    server->hosted_vms[server->num_hosted_vms] = NULL;

    /* Remove também a VM do vetor da reserva, mantendo a lista compacta. */
    memmove(&reservation->vms[reservation_index],
            &reservation->vms[reservation_index + 1],
            (reservation->num_vms - reservation_index - 1)
                * sizeof(reservation->vms[0]));

    reservation->num_vms--;

    /* Limpa a última posição, que já não está em utilização. */
    reservation->vms[reservation->num_vms] = NULL;

    /*
     * Remove a diretoria da VM e todo o seu conteúdo.
     * Se a remoção falhar, a função regista o erro, mas continua
     * a libertação da estrutura da VM.
     */
    char vm_path[MAX_PATH_SIZE];

    int n = snprintf(vm_path,sizeof(vm_path),"/tmp/CloudIST-ist187532/%s/%s",reservation->id,vm->id);

    if (n < 0 || (size_t)n >= sizeof(vm_path)) {
        fprintf(stderr,"VM path too long for %s.\n",vm->id);
    }
    
    else if (remove_path_recursive(vm_path) != 0) {
        perror("Failed to remove VM directory");
    }

    /* Liberta a memória ocupada pela estrutura da VM. */
    free(vm);

    return 0;
}