#ifndef VM_PROCESS_H
#define VM_PROCESS_H

#include "datacenter_utils.h"

/**
 * Configura os limites de recursos de uma VM e executa a sua aplicação
 * através do cpulimit. Esta função deve ser chamada no processo-filho.
 *
 * @param vm VM cuja aplicação será executada.
 * @param total_physical_cpus Número total de CPUs físicos usado no cálculo
 *                             do limite de CPU.
 * @param local_cores Número de núcleos locais considerado no cálculo.
 * @param executable_path Caminho do executável a iniciar.
 *
 * A função termina o processo com um código de erro se não conseguir
 * configurar os limites ou iniciar o executável.
 */
void spawn_vm_child(VM *vm, size_t total_physical_cpus, size_t local_cores, const char *executable_path);

/**
 * Termina e recolhe os processos criados durante uma tentativa de arranque
 * parcial, restaurando o estado das VMs para RESERVED.
 *
 * @param started Vetor com referências para as VMs iniciadas.
 * @param started_count Número de VMs registadas nesse vetor.
 */
void rollback_spawned_vms(VM **started, size_t started_count);

/**
 * Cria os processos das VMs reservadas e regista os respetivos PID e estado.
 *
 * Cada processo-filho prepara a diretoria de trabalho, redireciona a saída
 * padrão e inicia a aplicação com os limites de recursos definidos.
 *
 * @param res Reserva cujas VMs devem ser iniciadas.
 * @param total_physical_cpus Número total de CPUs físicos usado no cálculo
 *                             do limite de CPU.
 * @param local_cores Número de núcleos locais considerado no cálculo.
 *
 * @return 0 se a tentativa de arranque for concluída com sucesso.
 * @return 1 se os argumentos forem inválidos, não existirem VMs ou ocorrer
 *           uma falha na criação dos processos.
 */
int spawn_all_vms(Reservation *res, size_t total_physical_cpus, size_t local_cores);

/**
 * Verifica os processos de VMs em execução e recolhe os que terminaram.
 *
 * Atualiza o estado das VMs terminadas, liberta os seus recursos e remove
 * as reservas que já não contenham VMs.
 *
 * @param dc Centro de dados cujas reservas e VMs serão verificadas.
 *
 * @return 0 se não forem detetados erros.
 * @return 1 se ocorrer algum erro durante a verificação ou limpeza.
 */
int reap_finished_vms(DataCenter *dc);

/**
 * Liberta os recursos associados a uma VM terminada e remove-a dos vetores
 * da reserva e do servidor. Também tenta remover a diretoria da VM.
 *
 * A VM tem de estar no estado TERMINATED e ter PID igual a zero.
 *
 * @param reservation Reserva à qual a VM pertence.
 * @param vm VM a destruir.
 *
 * @return 0 se a estrutura da VM for libertada.
 * @return 1 se os argumentos ou as associações da VM forem inválidos,
 * ou se a VM ainda não tiver sido confirmada como terminada.
 */
int vm_destroy(Reservation *reservation, VM *vm);

#endif // VM_PROCESS_H