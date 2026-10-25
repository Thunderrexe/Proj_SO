#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include "parser.h"
#include "datacenter.h"
#include "datacenter_utils.h"
#include "constants.h"
#include "filesystem.h"
#include "vm_process.h"
#include <stdint.h>
#include <pthread.h>

/*
 * Verifica se ainda existe alguma VM em execução.
 * É usada no fim do programa, depois de todas as threads trabalhadoras
 * terminarem, para decidir se o processo principal ainda tem de esperar.
 */
static int has_running_vms(const DataCenter *dc) {
    for (size_t i = 0; i < dc->num_reservations; i++) {
        const Reservation *res = &dc->reservations[i];

        for (size_t j = 0; j < res->num_vms; j++) {
            const VM *vm = res->vms[j];

            if (vm != NULL && vm->state == VM_STATE_RUNNING) {
                return 1;
            }
        }
    }

    return 0;
}

/*
 * Processa sequencialmente os comandos de um ficheiro de configuração.
 * Cada worker trata um ficheiro de cada vez; os dados temporários dos
 * comandos são locais a esta função, enquanto o DataCenter é partilhado.
 */
static int process_config_file(
    DataCenter *dc,
    const char *path,
    size_t total_physical_cpus,
    size_t local_cores,
    pthread_mutex_t *dc_mutex
) 

{
    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        perror(path);
        return 1;
    }

    Command command;

    while ((command = get_next_command(fd)) != EOC) {
        int lock_result = pthread_mutex_lock(dc_mutex);
        if (lock_result != 0) {
            fprintf(stderr,"Failed to lock DataCenter mutex: %d\n",lock_result);
            close(fd);
            return 1;
        }
        
        if (reap_finished_vms(dc) != 0) {
            fprintf(stderr,
                    "Warning: some VM cleanup operations failed.\n");
        }

        /* Os comandos que consultam ou alteram o DataCenter são executados
         * dentro da secção protegida. Os erros saem do switch por break para
         * chegar sempre ao pthread_mutex_unlock() abaixo. */
        switch (command) {
            case CMD_DEFINE: {
                VMType vmtype;

                if (parse_define(fd, &vmtype) != 0) {
                    fprintf(stderr, "Invalid define command.\n");
                    break;
                }

                if (datacenter_define_VM(dc, &vmtype) != 0) {
                    fprintf(stderr, "Failed to define VM.\n");
                    break;
                }

                printf("VM successfully defined!\n");
                break;
            }

            case CMD_RESERVE: {
                Reservation reservation = {0};

                size_t num_items = parse_reserve(
                    fd,
                    &reservation,
                    MAX_RESERVATIONS_ITEMS
                );

                if (num_items == 0) {
                    fprintf(stderr, "Invalid reserve command.\n");
                    break;
                }

                if (datacenter_reserve(dc, &reservation) != 0) {
                    fprintf(stderr, "Failed to reserve VMs.\n");
                    break;
                }
                
                printf("Reservation made successfully!\n");
                break;
            }

            case CMD_EXECUTE: {
                char id[MAX_STRING_SIZE];

                if (parse_execute(fd, id) != 0) {
                    fprintf(stderr, "Invalid execute command.\n");
                    break;
                }

                if (datacenter_execute(
                        dc,
                        id,
                        total_physical_cpus,
                        local_cores
                    ) != 0) {
                    fprintf(stderr, "Failed to execute reservation.\n");
                    break;
                }
                printf("Finished reservation execution!\n");
                break;
            }

            case CMD_LIST:
                if (datacenter_list(dc) != 0) {
                    fprintf(stderr, "Failed to list VMs.\n");
                }
                break;

            case CMD_WAIT: {
                unsigned int delay;

                if (parse_wait(fd, &delay) != 0) {
                    fprintf(stderr, "Invalid wait command.\n");
                    break;
                }

                datacenter_wait(dc, delay);
                break;
            }

            case CMD_INVALID:
                fprintf(stderr, "Invalid command.\n");
                break;

            case CMD_HELP:
                printf(
                    "Available commands:\n"
                    " D <VM_TYPE_ID> <INPUT_FOLDER> "
                    "<EXECUTABLE_PATH> <RAM_NEEDED> "
                    "<DISK_NEEDED> <VCPU_NEEDED_COUNT>\n"
                    " R <RESERVATION_ID> "
                    "[<VM_TYPE_ID> <COUNT> <SERVER_ID>]+\n"
                    " A <RESERVATION_ID>\n"
                    " L\n"
                    " E <DELAY_MS>\n"
                    " H\n"
                );
                break;

            case CMD_EMPTY:
                break;

            case EOC:
                break;
        }
        int unlock_result = pthread_mutex_unlock(dc_mutex);
            if (unlock_result != 0) {
                fprintf(stderr,"Failed to unlock DataCenter mutex: %d\n",unlock_result);
                close(fd);
                return 1;
            }
    }

    if (close(fd) != 0) {
        perror("close config file");
        return 1;
    }

    return 0;
}

/*
 * Estado partilhado pelas threads trabalhadoras. queue_mutex protege apenas
 * next_file; dc_mutex protege as operações sobre o DataCenter.
 */
typedef struct {
    DataCenter *dc;
    ConfigFiles *config_files;
    size_t next_file;
    size_t total_physical_cpus;
    size_t local_cores;
    pthread_mutex_t queue_mutex;
    pthread_mutex_t dc_mutex;
} WorkerContext;

/*
 * Cada worker reclama o próximo ficheiro sob queue_mutex e processa-o depois
 * de libertar esse mutex. Assim, a fila só fica bloqueada durante a atribuição
 * do trabalho, e não durante a leitura e execução dos comandos.
 */
static void *worker_thread(void *arg) {
    WorkerContext *ctx = arg;

    while (1) {
        int lock_result = pthread_mutex_lock(&ctx->queue_mutex);

        if (lock_result != 0) {
            fprintf(stderr,
                    "Failed to lock queue mutex: %d\n",
                    lock_result);
            return NULL;
        }

        /* Não há mais ficheiros. Libertar a fila antes de terminar a thread. */
        if (ctx->next_file >= ctx->config_files->count) {
            (void)pthread_mutex_unlock(&ctx->queue_mutex);
            break;
        }

        /* Reclama um único ficheiro; o incremento ocorre protegido pelo mutex,
         * para que dois workers nunca processem o mesmo índice. */
        const char *path = ctx->config_files->paths[ctx->next_file];

        ctx->next_file++;

        int unlock_result =
            pthread_mutex_unlock(&ctx->queue_mutex);

        if (unlock_result != 0) {
            fprintf(stderr,
                    "Failed to unlock queue mutex: %d\n",
                    unlock_result);
            return NULL;
        }

        if (process_config_file(
                ctx->dc,
                path,
                ctx->total_physical_cpus,
                ctx->local_cores,
                &ctx->dc_mutex) != 0) {
            fprintf(stderr,
                    "Failed to process configuration file: %s\n",
                    path);
        }
    }

    return NULL;
}


int main(int argc, char **argv) {

    size_t max_threads;
    size_t local_cores;
    DataCenter dc;
    datacenter_init(&dc);

    if (argc != 8) {
        fprintf(stderr,"Usage: %s <servers> <ram> <disk> <cpus> <local_cores> <input_dir> <max_threads>\n", argv[0]);
        return 1;
    }

    size_t servers;
    size_t ram;
    size_t disk;
    size_t cpu;

    if (parse_size_t_arg(argv[1], &servers) != 0 ||
        parse_size_t_arg(argv[2], &ram) != 0 ||
        parse_size_t_arg(argv[3], &disk) != 0 ||
        parse_size_t_arg(argv[4], &cpu) != 0) {

        fprintf(stderr, "Invalid command line arguments.\n");
        return 1;
    }

    if (parse_size_t_arg(argv[5], &local_cores) != 0 || local_cores == 0) {
        fprintf(stderr, "Invalid local CPU count.\n");
        return 1;
    }

    if (parse_size_t_arg(argv[7], &max_threads) != 0 || max_threads == 0) {
        fprintf(stderr, "Invalid maximum thread count.\n");
        return 1;
    }

    /* Impede pedir mais cores locais do que aqueles que o sistema reporta. */
    long online_cpus = sysconf(_SC_NPROCESSORS_ONLN);

    if (online_cpus > 0 && local_cores > (size_t)online_cpus) {
        fprintf(stderr,"LOCAL_CORES exceeds the available online CPUs.\n");
        return 1;
    }

    /* Validar a configuração e verificar overflow antes de multiplicar.
     * O total é usado pelos processos VM para calcular os limites de CPU. */    
    if (servers == 0 || cpu == 0 || servers > SIZE_MAX / cpu) {
        fprintf(stderr, "Invalid server or CPU configuration.\n");
        return 1;
    }

    size_t total_physical_cpus = servers * cpu;

    /* Configurar os recursos do centro de dados antes de iniciar workers,
     * para que todas as threads encontrem uma estrutura já inicializada. */    
    Resources resources = {
        .ram = ram,
        .disk = disk,
        .cpu = cpu
    };

    if (datacenter_configure(&dc, servers, &resources) != 0) {
        fprintf(stderr, "Failed to configure Data Center.\n");
        return 1;
    }

    /* Descobrir os ficheiros antes de criar threads; a lista é partilhada
     * e não deve ser modificada enquanto os workers a estiverem a usar. */
    ConfigFiles config_files = {0};

    if (find_config_files(argv[6], &config_files) != 0) {
        fprintf(stderr, "Failed to find configuration files.\n");
        datacenter_destroy(&dc);
        return 1;
    }

    WorkerContext ctx = {
    .dc = &dc,
    .config_files = &config_files,
    .next_file = 0,
    .total_physical_cpus = total_physical_cpus,
    .local_cores = local_cores
    };

    int init_result = pthread_mutex_init(&ctx.queue_mutex, NULL);

    if (init_result != 0) {
        fprintf(stderr, "Failed to initialize queue mutex: %d\n", init_result);
        datacenter_destroy(&dc);
        return 1;
    }

    init_result = pthread_mutex_init(&ctx.dc_mutex, NULL);

    if (init_result != 0) {
        fprintf(stderr,"Failed to initialize DataCenter mutex: %d\n",init_result);
        (void)pthread_mutex_destroy(&ctx.queue_mutex);
        datacenter_destroy(&dc);
        return 1;
    }

    /* Não criar mais workers do que ficheiros disponíveis: threads adicionais
     * ficariam sem trabalho para executar. */
    size_t thread_count = max_threads;

    if (thread_count > config_files.count) {
        thread_count = config_files.count;
    }

    pthread_t *threads = NULL;

    if (thread_count > 0) {
        threads = calloc(thread_count, sizeof(*threads));

        if (threads == NULL) {
            perror("calloc threads");
            (void)pthread_mutex_destroy(&ctx.queue_mutex);
            (void)pthread_mutex_destroy(&ctx.dc_mutex);
            datacenter_destroy(&dc);
            return 1;
        }
    }

    /* Pode haver uma falha a meio da criação. Se pelo menos uma thread arrancar,
     * as threads criadas continuam a retirar ficheiros da fila partilhada. */
    size_t created = 0;

    for (; created < thread_count; created++) {
        int create_result = pthread_create(&threads[created],NULL,worker_thread,&ctx);

        if (create_result != 0) {
            fprintf(stderr,"Failed to create worker thread: %d\n",create_result);
            break;
        }
    }

    /* Se nenhuma thread puder ser criada, preservar a funcionalidade usando
     * processamento sequencial. Se algumas foram criadas, elas próprias
     * conseguem processar toda a fila antes de serem reunidas com join. */
    if (thread_count > 0 && created == 0) {
        fprintf(stderr,"No worker threads created; processing files sequentially.\n");

        for (size_t i = 0; i < config_files.count; i++) {
            if (process_config_file(&dc,config_files.paths[i],total_physical_cpus,local_cores,&ctx.dc_mutex) != 0) {
                fprintf(stderr,"Failed to process configuration file: %s\n",config_files.paths[i]);
            }
        }
    } 

    else {
        for (size_t i = 0; i < created; i++) {
            int join_result = pthread_join(threads[i], NULL);

            if (join_result != 0) {
                fprintf(stderr,"Failed to join worker thread: %d\n",join_result);
            }
        }
    }

    free(threads);

    /* Depois dos join, já não há workers a usar estes mutexes; podem ser
     * destruídos antes de o main tratar das VMs que ainda estejam ativas. */
    (void)pthread_mutex_destroy(&ctx.queue_mutex);
    (void)pthread_mutex_destroy(&ctx.dc_mutex);

    /*
     * As threads já terminaram de ler os comandos, mas as VMs lançadas por
     * elas podem continuar a correr. O main mantém-se ativo e espera que
     * terminem, recolhendo os processos e libertando os respetivos recursos.
     */
    while (has_running_vms(&dc)) {
        datacenter_wait(&dc, 10);
    }

    
    /* Última recolha antes de destruir o DataCenter, para tratar eventuais
     * processos terminados entre a última espera e a saída do ciclo. */
    if (reap_finished_vms(&dc) != 0) {
        fprintf(stderr,"Warning: some final VM cleanup operations failed.\n");
    }

    datacenter_destroy(&dc);

    return 0;
}