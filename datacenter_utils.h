#ifndef DATACENTER_UTILS__H
#define DATACENTER_UTILS__H

#include "datacenter.h"

/**
 * Procura um tipo de VM pelo identificador indicado.
 *
 * @param dc Ponteiro para o centro de dados.
 * @param type_id Identificador do tipo de VM a procurar.
 *
 * @return Ponteiro para o tipo de VM, caso exista.
 * @return NULL se não existir nenhum tipo de VM com esse identificador.
 */
VMType *VMType_exists(DataCenter *dc, const char* type_id);

/**
 * Valida a alocação de todas as VMs solicitadas numa reserva, utilizando
 * cópias temporárias dos recursos disponíveis e do número de VMs alojadas
 * em cada servidor. O estado real do centro de dados não é alterado.
 *
 * A validação falha se os parâmetros forem inválidos, se algum tipo de VM
 * não existir, se um identificador de servidor for inválido, se forem
 * ultrapassados os limites de VMs ou se não existirem recursos suficientes.
 *
 * @param dc Centro de dados onde a reserva será validada.
 * @param reservation Reserva a validar.
 *
 * @return 0 se todas as VMs puderem ser alocadas.
 * @return 1 se ocorrer algum erro de validação.
 */
int reservation_validate(DataCenter *dc, Reservation *reservation);

/**
 * Cria todas as VMs solicitadas pela reserva, associa-as aos servidores
 * correspondentes, atualiza os recursos disponíveis e regista as VMs
 * tanto na reserva como nas listas de VMs alojadas nos servidores.
 *
 * A reserva deve ter sido validada antes desta função ser chamada. Se
 * ocorrer um erro durante a criação das VMs, as alterações efetuadas
 * pela operação são revertidas.
 *
 * @param dc Centro de dados onde a reserva será concretizada.
 * @param reservation Reserva cujas VMs serão criadas.
 *
 * @return 0 se a reserva for concretizada com sucesso.
 * @return 1 se ocorrer um erro e as alterações forem revertidas.
 */
int reservation_commit(DataCenter *dc, Reservation *reservation);

/**
 * Procura uma reserva pelo identificador e verifica se está pendente.
 *
 * @param dc Ponteiro para o centro de dados.
 * @param reservation_id Identificador da reserva a procurar.
 *
 * @return Ponteiro para a reserva, caso exista e esteja no estado pendente.
 * @return NULL se a reserva não existir ou não estiver pendente. Neste caso,
 * é apresentada uma mensagem de erro.
 */
Reservation *find_pending_reservation(DataCenter *dc, const char *reservation_id);

/**
 * Destrói uma reserva, libertando as VMs associadas, devolvendo os recursos
 * utilizados aos respetivos servidores e removendo as VMs das listas dos
 * servidores. Remove também os ficheiros da reserva e o respetivo registo
 * da lista de reservas do centro de dados.
 *
 * @param dc Centro de dados que contém a reserva.
 * @param reservation Ponteiro para a reserva a destruir.
 */
void reservation_destroy(DataCenter *dc, Reservation *reservation);

/**
 * Remove uma reserva concluída do centro de dados e elimina a respetiva
 * diretoria e o seu conteúdo no sistema de ficheiros.
 *
 * A reserva tem de ser válida, pertencer ao centro de dados e não ter
 * VMs associadas. Se a remoção da diretoria falhar, o registo da reserva
 * não é removido.
 *
 * @param dc Centro de dados que contém a reserva.
 * @param res Reserva concluída a remover.
 *
 * @return 0 se a reserva for removida com sucesso.
 * @return 1 se os argumentos forem inválidos, a reserva não for encontrada
 *         ou ocorrer um erro durante a remoção.
 */
int remove_completed_reservation(DataCenter *dc, Reservation *res);

/**
 * Remove recursivamente um caminho do sistema de ficheiros.
 *
 * Se o caminho corresponder a uma diretoria, remove primeiro o seu conteúdo
 * e, em seguida, a própria diretoria. Os ficheiros e as ligações simbólicas
 * são removidos diretamente. Um caminho que já não exista é considerado
 * uma operação concluída com sucesso.
 *
 * @param path Caminho do ficheiro ou da diretoria a remover.
 *
 * @return 0 se a remoção for concluída com sucesso.
 * @return -1 se ocorrer algum erro.
 */
int remove_path_recursive(const char *path);


/**
 * Converte o estado de uma VM numa cadeia de caracteres legível.
 * Esta função é utilizada por datacenter_list() para apresentar o estado.
 *
 * @param state Estado da VM a converter.
 *
 * @return Cadeia de caracteres correspondente ao estado.
 * Devolve "UNKNOWN" se o estado não for reconhecido.
 */
const char *vm_state_to_string(VMState state);

/**
 * Converte o estado de uma reserva numa cadeia de caracteres legível.
 * Esta função é utilizada por datacenter_list() para apresentar o estado.
 *
 * @param state Estado da reserva a converter.
 *
 * @return Cadeia de caracteres correspondente ao estado.
 * Devolve "UNKNOWN" se o estado não for reconhecido.
 */
const char *res_state_to_string(ReservationState state);

#endif // DATACENTER_UTILS__H