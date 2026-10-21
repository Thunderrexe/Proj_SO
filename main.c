#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include "parser.h"
#include "datacenter.h"
#include "datacenter_utils.h"
#include "constants.h"
#include "filesystem.h"
#include <stdint.h>

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



int main(int argc, char **argv) {

    size_t local_cores;
    DataCenter dc;
    datacenter_init(&dc);

    if (argc != 7) {
        fprintf(stderr,"Usage: %s <servers> <ram> <disk> <cpus> <local_cores> <input_dir>\n", argv[0]);
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

    long online_cpus = sysconf(_SC_NPROCESSORS_ONLN);

    if (online_cpus > 0 && local_cores > (size_t)online_cpus) {
        fprintf(stderr,"LOCAL_CORES exceeds the available online CPUs.\n");
        return 1;
    }

    /* Validar a configuração e calcular o total de cores físicos. */
    if (servers == 0 || cpu == 0 || servers > SIZE_MAX / cpu) {
        fprintf(stderr, "Invalid server or CPU configuration.\n");
        return 1;
    }

    size_t total_physical_cpus = servers * cpu;

    /* Configurar os recursos do centro de dados. */
    Resources resources = {
        .ram = ram,
        .disk = disk,
        .cpu = cpu
    };

    if (datacenter_configure(&dc, servers, &resources) != 0) {
        fprintf(stderr, "Failed to configure Data Center.\n");
        return 1;
    }

    ConfigFiles config_files = {0};

    if (find_config_files(argv[6], &config_files) != 0) {
        fprintf(stderr, "Failed to find configuration files.\n");
        datacenter_destroy(&dc);
        return 1;
    }

    for (size_t i = 0; i < config_files.count; i++) {

        int fd = open(config_files.paths[i], O_RDONLY);

        if (fd < 0) {
            perror("open");
            continue;
        }

        Command command;

        while ((command = get_next_command(fd)) != EOC) {

        /*
         * Recolher processos terminados e libertar os
         * respetivos recursos antes do próximo comando.
         */
            if (reap_finished_vms(&dc) != 0) {
                fprintf(stderr,"Warning: some VM cleanup operations failed.\n");
            }

    switch (command) {

                case CMD_DEFINE: { // D <TIPO_VM> <DIR_INPUTS> <CAMINHO_EXEC> <RAM> <DISCO> <VCPU>
                    VMType vmtype;

                    if (parse_define(fd, &vmtype) != 0) {
                        fprintf(stderr, "Invalid define command.\n");
                        continue;
                    }

                    if (datacenter_define_VM(&dc, &vmtype) != 0) {
                        fprintf(stderr, "Failed to define VM.\n");
                        continue;
                    }

                    break;
                }

                case CMD_RESERVE: { // R <ID_RESERVA> [<TIPO_VM> <QTD> <ID_SERVIDOR>]+
                    Reservation reservation = {0};

                    size_t num_items = parse_reserve(fd, &reservation, MAX_RESERVATIONS_ITEMS);

                    if (num_items == 0) {
                        fprintf(stderr, "Invalid reserve command.\n");
                        continue;
                    }

                    if (datacenter_reserve(&dc, &reservation) != 0) {
                        fprintf(stderr, "Failed to reserve VMs.\n");
                        continue;
                    }

                    break;
                }

                case CMD_EXECUTE: { // A <ID_RESERVA>
                    char id[MAX_STRING_SIZE];

                    if (parse_execute(fd, id) != 0) {
                        fprintf(stderr, "Invalid execute command.\n");
                        continue;
                    }

                    if (datacenter_execute(&dc, id, total_physical_cpus,local_cores) != 0) {
                        fprintf(stderr,"Failed to execute reservation.\n");
                        continue;
                    }

                    break;
                }

                case CMD_LIST: // L

                    if (datacenter_list(&dc) != 0) {
                        fprintf(stderr, "Failed to list VMs.\n");
                    }

                    break;

                case CMD_WAIT: { // E <DELAY_MS>
                    unsigned int delay;

                    if (parse_wait(fd, &delay) != 0) {
                        fprintf(stderr, "Invalid wait command.\n");
                        continue;
                    }

                    datacenter_wait(&dc, delay);
                    break;
                }

                case CMD_INVALID: // Invalid command
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

        }

        close(fd);
    }

    /*
     * Continuar a monitorizar as VMs ativas até terminarem.
     */
    while (has_running_vms(&dc)) {
        datacenter_wait(&dc, 10);
    }

    /*
     * Uma última verificação para tentar concluir eventuais
     * operações de limpeza pendentes.
     */

    if (reap_finished_vms(&dc) != 0) {
        fprintf(stderr,"Warning: some final VM cleanup operations failed.\n");
    }

    datacenter_destroy(&dc);

    return 0;
}