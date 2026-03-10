/* ============================================================
 * PROGRAMA PRINCIPAL — main.c
 * Projeto OWR (OverlayWithRouting)
 *
 * Este ficheiro contém o ponto de entrada do programa e toda
 * a lógica de controlo de eventos (event loop), tratamento
 * de comandos do utilizador, protocolos de rede sobreposta
 * e protocolo de encaminhamento sem ciclos.
 *
 * Arquitetura geral:
 *   O programa corre um único ciclo principal (loop select)
 *   que monitoriza em simultâneo:
 *     - stdin          : comandos do utilizador
 *     - fd_udp         : respostas do servidor de nós (UDP)
 *     - fd_tcp_listen  : novas ligações TCP de entrada
 *     - vizinhos[i].fd : mensagens dos vizinhos já conectados
 *
 * Protocolos implementados:
 *   - Rede sobreposta : NEIGHBOR
 *   - Encaminhamento  : ROUTE, COORD, UNCOORD
 *
 * Comandos suportados (interface do utilizador):
 *   join / j              — entrar na rede (com registo UDP)
 *   dj / direct join      — entrar na rede (sem UDP, local)
 *   leave / l             — sair da rede (desregisto UDP)
 *   exit / x              — fechar o programa
 *   show nodes / n        — listar nós da rede via servidor
 *   add edge / ae         — ligar a vizinho via servidor UDP
 *   dae / direct add edge — ligar a vizinho diretamente (IP+porto)
 *   remove edge / re      — remover aresta com vizinho
 *   show neighbors / sg   — listar vizinhos atuais
 *   show routing / sr     — mostrar tabela de encaminhamento
 *   announce / a          — anunciar este nó à rede
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include "rede.h"


/* ============================================================
 * verificar_fim_coordenacao
 *
 * Verifica se todos os vizinhos já responderam à mensagem
 * COORD para um dado destino d. Se sim, termina o estado de
 * coordenação e retoma o encaminhamento normal.
 *
 * Esta função é chamada sempre que um vizinho responde com
 * UNCOORD ou quando uma ligação cai durante a coordenação.
 *
 * LÓGICA DE TÉRMINO DE COORDENAÇÃO:
 *   1. Percorre coord[0..MAX_NEIGHBORS-1]. Se algum for 1,
 *      ainda há vizinhos a responder — sai sem fazer nada.
 *   2. Se todos forem 0:
 *      a) Volta ao estado de EXPEDIÇÃO (state = 0).
 *      b) Se encontrou uma rota durante a coordenação
 *         (dist != -1), anuncia-a a todos os vizinhos com ROUTE.
 *      c) Se esta coordenação foi desencadeada por outro nó
 *         (succ_coord != '?' e != '-1'), envia UNCOORD de volta
 *         a esse nó para o informar que já não depende dele.
 *
 * Parâmetros:
 *   d               — índice do destino na tabela de rotas.
 *   vizinhos[]      — array de vizinhos.
 *   tabela_rotas[]  — tabela de encaminhamento.
 * ============================================================ */
void verificar_fim_coordenacao(int d, struct Vizinho vizinhos[], struct Rota tabela_rotas[]) {
    /* Verifica se ainda há vizinhos à espera de resposta.
     * coord[k] == 1 significa que o vizinho k ainda não respondeu. */
    for (int k = 0; k < MAX_NEIGHBORS; k++) {
        if (tabela_rotas[d].coord[k] == 1) return; /* Ainda à espera — não terminar. */
    }

    /* Todos responderam: termina a coordenação e regressa a EXPEDIÇÃO. */
    tabela_rotas[d].state = 0;

    /* Se encontrou uma rota válida durante a coordenação,
     * anuncia-a a todos os vizinhos com mensagem ROUTE. */
    if (tabela_rotas[d].dist != -1) {
        char msg[128];
        sprintf(msg, "ROUTE %02d %d\n", d, tabela_rotas[d].dist);
        for (int k = 0; k < MAX_NEIGHBORS; k++) {
            if (vizinhos[k].fd != -1) write(vizinhos[k].fd, msg, strlen(msg));
        }
    }

    /* Se esta coordenação foi causada por outro nó (não por quebra direta),
     * envia UNCOORD de volta a esse nó para fechar o ciclo de coordenação.
     * succ_coord == '?'  → coordenação iniciada localmente (sem UNCOORD a enviar).
     * succ_coord == '-1' → quebra direta de ligação (sem nó a notificar). */
    if (strcmp(tabela_rotas[d].succ_coord, "?") != 0 &&
        strcmp(tabela_rotas[d].succ_coord, "-1") != 0) {

        /* Encontra o fd do vizinho que causou a coordenação pelo seu ID. */
        int fd_succ = -1;
        for (int k = 0; k < MAX_NEIGHBORS; k++) {
            if (vizinhos[k].fd != -1 &&
                strcmp(vizinhos[k].id, tabela_rotas[d].succ_coord) == 0) {
                fd_succ = vizinhos[k].fd;
                break;
            }
        }

        /* Envia UNCOORD para informar que já não dependemos desse vizinho. */
        if (fd_succ != -1) {
            char umsg[128];
            sprintf(umsg, "UNCOORD %02d\n", d);
            write(fd_succ, umsg, strlen(umsg));
        }
    }
}


/* ============================================================
 * tratar_falha_ligacao
 *
 * Reage à queda de uma ligação TCP com o vizinho no slot
 * slot_removido. Aplica o protocolo sem ciclos a todos os
 * destinos afetados.
 *
 * É chamada em dois cenários:
 *   - Queda espontânea: read() devolve 0 ou -1 (nó desligou-se).
 *   - Remoção manual:   utilizador executa "remove edge".
 *
 * LÓGICA (por cada destino d na tabela de rotas):
 *
 *   Caso 1 — Estado EXPEDIÇÃO e rota passa pelo vizinho perdido:
 *     O nó já não consegue alcançar d por este caminho.
 *     → Entra em COORDENAÇÃO: dist=-1, succ='?', envia COORD
 *       a todos os outros vizinhos e aguarda as suas respostas.
 *     → succ_coord = '-1' (coordenação por quebra direta, não
 *       por pedido de outro nó).
 *
 *   Caso 2 — Estado COORDENAÇÃO e aguardava resposta deste vizinho:
 *     O vizinho que caiu nunca vai responder com UNCOORD.
 *     → Marca coord[slot_removido] = 0 (considera como respondido)
 *       e verifica se a coordenação pode terminar.
 *
 * Parâmetros:
 *   slot_removido   — índice no array vizinhos do nó que caiu.
 *   vizinhos[]      — array de vizinhos.
 *   tabela_rotas[]  — tabela de encaminhamento.
 * ============================================================ */
void tratar_falha_ligacao(int slot_removido, struct Vizinho vizinhos[], struct Rota tabela_rotas[]) {
    /* Guarda o ID antes de o slot ser limpo, para poder identificar
     * as rotas que passavam por este vizinho. */
    char vizinho_id[3];
    strcpy(vizinho_id, vizinhos[slot_removido].id);
    int afetadas = 0;

    for (int d = 0; d < 100; d++) {

        /* CASO 1: Em expedição e a rota para d passava pelo vizinho perdido. */
        if (tabela_rotas[d].state == 0 &&
            strcmp(tabela_rotas[d].succ, vizinho_id) == 0) {

            /* Entra em coordenação: invalida a rota atual. */
            tabela_rotas[d].state = 1;
            strcpy(tabela_rotas[d].succ_coord, "-1"); /* '-1' = quebra direta, sem nó a notificar no fim */
            tabela_rotas[d].dist = -1;
            strcpy(tabela_rotas[d].succ, "?");

            char coord_msg[128];
            sprintf(coord_msg, "COORD %02d\n", d);

            /* Envia COORD a todos os vizinhos restantes (exceto o que caiu)
             * e marca cada um como "à espera de resposta". */
            for (int k = 0; k < MAX_NEIGHBORS; k++) {
                if (vizinhos[k].fd != -1 && k != slot_removido) {
                    tabela_rotas[d].coord[k] = 1;
                    write(vizinhos[k].fd, coord_msg, strlen(coord_msg));
                }
            }

            /* Verifica imediatamente se já não há vizinhos a aguardar
             * (pode acontecer se este era o único vizinho). */
            verificar_fim_coordenacao(d, vizinhos, tabela_rotas);
            afetadas++;
        }
        /* CASO 2: Já em coordenação e aguardávamos resposta deste vizinho.
         * Como ele caiu, nunca vai responder — considera-o como respondido. */
        else if (tabela_rotas[d].state == 1) {
            if (tabela_rotas[d].coord[slot_removido] == 1) {
                tabela_rotas[d].coord[slot_removido] = 0;
                verificar_fim_coordenacao(d, vizinhos, tabela_rotas);
            }
        }
    }

    if (afetadas > 0) {
        printf("[ROUTING] Ligação perdida: %d rota(s) dependente(s) do Nó %s"
               " entraram em Coordenação para evitar ciclos.\n", afetadas, vizinho_id);
    }
}


/* ============================================================
 * main
 *
 * Ponto de entrada do programa.
 *
 * FASES:
 *   1. Parsing de argumentos e configuração dos contactos.
 *   2. Criação dos sockets UDP (servidor de nós) e TCP (escuta).
 *   3. Inicialização das estruturas de vizinhos e rotas.
 *   4. Event loop (select): aguarda e processa eventos de
 *      stdin, UDP, TCP listen e mensagens de vizinhos.
 * ============================================================ */
int main(int argc, char *argv[]) {
    char meuIP[20], regIP[20];
    int meuTCP, regUDP;

    /* -------------------------------------------------------
     * FASE 1: Parsing de argumentos
     *
     * Uso: ./OWR IP TCP [regIP regUDP]
     *   IP, TCP   — contacto deste nó na internet.
     *   regIP     — IP do servidor de nós (default: 193.136.138.142).
     *   regUDP    — porto UDP do servidor de nós (default: 59000).
     * ------------------------------------------------------- */
    if (argc == 3) {
        /* Argumentos mínimos: usa valores default para o servidor. */
        strcpy(meuIP, argv[1]); meuTCP = atoi(argv[2]);
        strcpy(regIP, "193.136.138.142"); regUDP = 59000;
    } else if (argc == 5) {
        /* Argumentos completos: servidor de nós personalizado. */
        strcpy(meuIP, argv[1]); meuTCP = atoi(argv[2]);
        strcpy(regIP, argv[3]); regUDP = atoi(argv[4]);
    } else {
        printf("Uso: ./OWR IP TCP [regIP regUDP]\n"); exit(1);
    }

    printf("Bem-vindo ao OWR!\nO teu Nó -> IP: %s, Porto TCP: %d\n", meuIP, meuTCP);

    /* -------------------------------------------------------
     * FASE 2A: Socket UDP para comunicação com o servidor de nós
     *
     * UDP é não-orientado à ligação: não há connect()/accept().
     * Usamos sendto/recvfrom com o endereço do servidor em cada chamada.
     * ------------------------------------------------------- */
    int fd_udp = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in serveraddr;
    memset(&serveraddr, 0, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port   = htons(regUDP);
    inet_pton(AF_INET, regIP, &serveraddr.sin_addr);

    /* -------------------------------------------------------
     * FASE 2B: Socket TCP de escuta (servidor)
     *
     * Este socket aceita ligações de outros nós que queiram
     * criar uma aresta connosco.
     *
     * SO_REUSEADDR: permite reutilizar o porto imediatamente após
     * reiniciar o programa (evita "Address already in use").
     * INADDR_ANY: aceita ligações em qualquer interface de rede.
     * listen(fd, 5): fila de espera de até 5 ligações pendentes.
     * ------------------------------------------------------- */
    int fd_tcp_listen = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(fd_tcp_listen, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in tcp_addr;
    memset(&tcp_addr, 0, sizeof(tcp_addr));
    tcp_addr.sin_family      = AF_INET;
    tcp_addr.sin_port        = htons(meuTCP);
    tcp_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(fd_tcp_listen, (struct sockaddr*)&tcp_addr, sizeof(tcp_addr));
    listen(fd_tcp_listen, 5);

    /* -------------------------------------------------------
     * FASE 3: Inicialização das estruturas de dados
     * ------------------------------------------------------- */
    char buffer[256], minha_net[4] = "", meu_id[3] = "";
    int registado = 0, maxfd;  /* registado: flag de registo ativo no servidor */
    fd_set readfds;

    struct Vizinho vizinhos[MAX_NEIGHBORS];
    inicializar_vizinhos(vizinhos);    /* Todos os slots a fd=-1 */

    struct Rota tabela_rotas[100];
    inicializar_rotas(tabela_rotas);   /* Todas as rotas a dist=-1, state=0 */

    /* -------------------------------------------------------
     * FASE 4: Event loop principal
     *
     * select() bloqueia até haver atividade em pelo menos um
     * dos file descriptors monitorizados. Depois verificamos
     * qual(is) têm dados e processamos cada um.
     * ------------------------------------------------------- */
    while(1) {
        /* Reconstrói o fd_set em cada iteração (select modifica-o). */
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO,   &readfds);  /* Comandos do utilizador */
        FD_SET(fd_udp,         &readfds);  /* Respostas do servidor UDP */
        FD_SET(fd_tcp_listen,  &readfds);  /* Novas ligações TCP de entrada */

        /* Adiciona os fds dos vizinhos ativos e calcula o maxfd.
         * select() precisa do valor máximo de fd + 1. */
        maxfd = fd_udp > fd_tcp_listen ? fd_udp : fd_tcp_listen;
        for (int i = 0; i < MAX_NEIGHBORS; i++) {
            if (vizinhos[i].fd != -1) {
                FD_SET(vizinhos[i].fd, &readfds);
                if (vizinhos[i].fd > maxfd) maxfd = vizinhos[i].fd;
            }
        }

        printf("> "); fflush(stdout);  /* Prompt — fflush garante que aparece antes do select bloquear */

        /* Bloqueia até haver dados em algum fd. NULL = sem timeout. */
        if (select(maxfd + 1, &readfds, NULL, NULL, NULL) == -1) break;


        /* =======================================================
         * EVENTO 1: Comando do utilizador via stdin
         * ======================================================= */
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            if (fgets(buffer, sizeof(buffer), stdin) != NULL) {
                /* Remove o '\n' final para facilitar comparações. */
                buffer[strcspn(buffer, "\n")] = 0;

                /* Faz parse dos argumentos do comando (até 6 tokens). */
                char command[20]="", arg1[20]="", arg2[20]="", arg3[20]="", arg4[20]="", arg5[20]="";
                sscanf(buffer, "%s %s %s %s %s %s", command, arg1, arg2, arg3, arg4, arg5);

                /* --- exit / x : encerrar o programa --- */
                if (strcmp(command, "exit") == 0 || strcmp(command, "x") == 0) {
                    /* Se estiver registado, envia leave ao servidor antes de sair
                     * (op=3 = pedido de desregisto no protocolo do servidor). */
                    if (registado) {
                        char leave_msg[128];
                        sprintf(leave_msg, "REG 123 3 %s %s", minha_net, meu_id);
                        sendto(fd_udp, leave_msg, strlen(leave_msg), 0,
                               (struct sockaddr*)&serveraddr, sizeof(serveraddr));
                    }
                    printf("A fechar...\n"); break;
                }

                /* --- join / j : entrar na rede com registo no servidor UDP ---
                 * Envia REG com op=0 (registo). O servidor responde com op=1
                 * (confirmação) ou op=2 (rede cheia). */
                else if (strcmp(command, "join") == 0 || strcmp(command, "j") == 0) {
                    strcpy(minha_net, arg1); strcpy(meu_id, arg2); registado = 1;
                    char reg_msg[128];
                    sprintf(reg_msg, "REG 123 0 %s %s %s %d", minha_net, meu_id, meuIP, meuTCP);
                    sendto(fd_udp, reg_msg, strlen(reg_msg), 0,
                           (struct sockaddr*)&serveraddr, sizeof(serveraddr));
                }

                /* --- dj / direct join : entrar na rede SEM servidor UDP ---
                 * Útil para testes locais onde não há servidor disponível.
                 * Define net e id localmente sem qualquer troca UDP. */
                else if (strcmp(command, "dj") == 0) {
                    strcpy(minha_net, arg1); strcpy(meu_id, arg2); registado = 1;
                    printf("[LOCAL] Direct Join efetuado: Nó %s na rede %s (sem UDP).\n", meu_id, minha_net);
                }
                else if (strcmp(command, "direct") == 0 && strcmp(arg1, "join") == 0) {
                    strcpy(minha_net, arg2); strcpy(meu_id, arg3); registado = 1;
                    printf("[LOCAL] Direct Join efetuado: Nó %s na rede %s (sem UDP).\n", meu_id, minha_net);
                }

                /* --- dae / direct add edge : ligar diretamente a outro nó ---
                 * Recebe ID, IP e porto diretamente do utilizador, sem consultar
                 * o servidor. Útil para testes locais.
                 * Sintaxe: dae <id> <ip> <porto>
                 *          direct add edge <id> <ip> <porto> */
                else if (strcmp(command, "dae") == 0) {
                    ligar_vizinho(vizinhos, arg1, arg2, atoi(arg3), meu_id);
                }
                else if (strcmp(command, "direct") == 0 &&
                         strcmp(arg1, "add") == 0 && strcmp(arg2, "edge") == 0) {
                    ligar_vizinho(vizinhos, arg3, arg4, atoi(arg5), meu_id);
                }

                /* --- leave / l : sair da rede (desregisto no servidor) ---
                 * Envia REG com op=3. O nó deixa de aparecer nas listas do
                 * servidor, mas as ligações TCP existentes mantêm-se ativas
                 * até serem explicitamente removidas. */
                else if (strcmp(command, "leave") == 0 || strcmp(command, "l") == 0) {
                    if (registado) {
                        char leave_msg[128];
                        sprintf(leave_msg, "REG 123 3 %s %s", minha_net, meu_id);
                        sendto(fd_udp, leave_msg, strlen(leave_msg), 0,
                               (struct sockaddr*)&serveraddr, sizeof(serveraddr));
                        registado = 0;
                    }
                }

                /* --- show nodes / n : listar nós numa rede via servidor ---
                 * Envia NODES ao servidor, que responde com a lista de IDs. */
                else if (strcmp(command, "n") == 0 ||
                         (strcmp(command, "show") == 0 && strcmp(arg1, "nodes") == 0)) {
                    char target_net[4] = "";
                    if (strcmp(command, "n") == 0) strcpy(target_net, arg1);
                    else strcpy(target_net, arg2);
                    char nodes_msg[128];
                    sprintf(nodes_msg, "NODES 123 0 %s", target_net);
                    sendto(fd_udp, nodes_msg, strlen(nodes_msg), 0,
                           (struct sockaddr*)&serveraddr, sizeof(serveraddr));
                }

                /* --- add edge / ae : ligar a um vizinho via servidor UDP ---
                 * Envia CONTACT ao servidor pedindo o contacto (IP+porto) do
                 * nó alvo. Quando o servidor responder (CONTACT op=1), a
                 * ligação TCP é estabelecida no handler UDP abaixo. */
                else if ((strcmp(command, "add") == 0 && strcmp(arg1, "edge") == 0) ||
                          strcmp(command, "ae") == 0) {
                    char target_id[3] = "";
                    if (strcmp(command, "ae") == 0) strcpy(target_id, arg1);
                    else strcpy(target_id, arg2);
                    char contact_msg[128];
                    sprintf(contact_msg, "CONTACT 123 0 %s %s", minha_net, target_id);
                    sendto(fd_udp, contact_msg, strlen(contact_msg), 0,
                           (struct sockaddr*)&serveraddr, sizeof(serveraddr));
                }

                /* --- remove edge / re : remover aresta com um vizinho ---
                 * Primeiro notifica o protocolo de encaminhamento (para
                 * recalcular rotas afetadas), depois fecha a ligação TCP. */
                else if ((strcmp(command, "remove") == 0 && strcmp(arg1, "edge") == 0) ||
                          strcmp(command, "re") == 0) {
                    char target_id[3] = "";
                    if (strcmp(command, "re") == 0) strcpy(target_id, arg1);
                    else strcpy(target_id, arg2);

                    /* Encontra o slot do vizinho para passar ao tratador de falha. */
                    int slot = -1;
                    for (int i = 0; i < MAX_NEIGHBORS; i++) {
                        if (vizinhos[i].fd != -1 &&
                            strcmp(vizinhos[i].id, target_id) == 0) slot = i;
                    }
                    /* Desencadeia o protocolo sem ciclos ANTES de fechar o socket. */
                    if (slot != -1) tratar_falha_ligacao(slot, vizinhos, tabela_rotas);
                    remover_aresta(vizinhos, target_id);
                }

                /* --- show neighbors / sg : listar vizinhos atuais --- */
                else if (strcmp(command, "sg") == 0 ||
                         (strcmp(command, "show") == 0 && strcmp(arg1, "neighbors") == 0)) {
                    mostrar_vizinhos(vizinhos);
                }

                /* --- show routing / sr : mostrar tabela de encaminhamento ---
                 * Imprime todas as entradas com rota conhecida (dist != -1)
                 * ou em estado de coordenação (state == 1). */
                else if (strcmp(command, "sr") == 0 ||
                         (strcmp(command, "show") == 0 && strcmp(arg1, "routing") == 0)) {
                    printf("--- Tabela de Encaminhamento ---\n");
                    int encontrou = 0;
                    for (int i = 0; i < 100; i++) {
                        if (tabela_rotas[i].dist != -1 || tabela_rotas[i].state == 1) {
                            printf("-> Dest: %02d | Succ: %s | Dist: %d | Estado: %d | SuccCoord: %s\n",
                                   i, tabela_rotas[i].succ, tabela_rotas[i].dist,
                                   tabela_rotas[i].state, tabela_rotas[i].succ_coord);
                            encontrou++;
                        }
                    }
                    if (encontrou == 0) printf("A tabela de rotas está vazia.\n");
                    printf("--------------------------------\n");
                }

                /* --- announce / a : anunciar este nó à rede ---
                 * Define dist=0 para o próprio ID (estou a 0 saltos de mim mesmo)
                 * e envia ROUTE a todos os vizinhos para propagarem a informação. */
                else if (strcmp(command, "announce") == 0 || strcmp(command, "a") == 0) {
                    if (registado) {
                        int meu_indice = atoi(meu_id);
                        tabela_rotas[meu_indice].dist  = 0;
                        strcpy(tabela_rotas[meu_indice].succ, meu_id);
                        tabela_rotas[meu_indice].state = 0;

                        printf("[ROUTING] Anunciei-me! Distância para o Nó %s é 0.\n", meu_id);

                        char route_msg[128];
                        sprintf(route_msg, "ROUTE %s 0\n", meu_id);
                        for (int i = 0; i < MAX_NEIGHBORS; i++) {
                            if (vizinhos[i].fd != -1)
                                write(vizinhos[i].fd, route_msg, strlen(route_msg));
                        }
                    }
                }
            }
        } /* fim handler stdin */


        /* =======================================================
         * EVENTO 2: Resposta do servidor de nós via UDP
         *
         * O servidor responde a pedidos REG, NODES e CONTACT.
         * O único caso que requer ação automática é CONTACT op=1:
         * o servidor devolveu o contacto de um nó — ligamo-nos.
         * ======================================================= */
        if (FD_ISSET(fd_udp, &readfds)) {
            socklen_t addrlen = sizeof(serveraddr);
            int n = recvfrom(fd_udp, buffer, sizeof(buffer) - 1, 0,
                             (struct sockaddr*)&serveraddr, &addrlen);
            if (n > 0) {
                buffer[n] = '\0';
                printf("\n[SERVIDOR RESPONDEU]: %s\n", buffer);

                /* Tenta fazer parse de uma resposta CONTACT completa.
                 * Formato: CONTACT <tid> <op> <net> <id> <ip> <tcp>
                 * op=1 significa sucesso — o servidor enviou o contacto. */
                char res_type[20], res_net[4], res_id[3], res_ip[20];
                int res_tid, res_op, res_tcp;
                if (sscanf(buffer, "%s %d %d %s %s %s %d",
                           res_type, &res_tid, &res_op, res_net, res_id, res_ip, &res_tcp) == 7) {
                    if (strcmp(res_type, "CONTACT") == 0 && res_op == 1) {
                        /* Servidor respondeu com contacto válido: estabelece ligação TCP. */
                        ligar_vizinho(vizinhos, res_id, res_ip, res_tcp, meu_id);
                    }
                }
            }
        } /* fim handler UDP */


        /* =======================================================
         * EVENTO 3: Nova ligação TCP de entrada
         *
         * Outro nó quis criar uma aresta connosco.
         * aceitar_ligacao() aceita e guarda o fd com id='?'.
         * O id será atualizado quando chegar a mensagem NEIGHBOR.
         * ======================================================= */
        if (FD_ISSET(fd_tcp_listen, &readfds)) {
            aceitar_ligacao(fd_tcp_listen, vizinhos);
        }


        /* =======================================================
         * EVENTO 4: Mensagem recebida de um vizinho TCP
         *
         * Para cada vizinho ativo, verifica se tem dados.
         * Casos possíveis:
         *   n <= 0    → vizinho desligou-se (queda de ligação)
         *   NEIGHBOR  → apresentação: atualiza o id do slot
         *   ROUTE     → anúncio de rota: atualiza tabela se melhor
         *   COORD     → pedido de coordenação: protocolo sem ciclos
         *   UNCOORD   → resposta de coordenação: protocolo sem ciclos
         * ======================================================= */
        for (int i = 0; i < MAX_NEIGHBORS; i++) {
            if (vizinhos[i].fd == -1 || !FD_ISSET(vizinhos[i].fd, &readfds)) continue;

            int n = read(vizinhos[i].fd, buffer, sizeof(buffer) - 1);

            /* --- Queda de ligação ---
             * read() retorna 0 (FIN) ou negativo (erro).
             * Desencadeia o protocolo sem ciclos antes de limpar o slot. */
            if (n <= 0) {
                printf("\n[TCP] O Nó %s desligou-se.\n", vizinhos[i].id);
                tratar_falha_ligacao(i, vizinhos, tabela_rotas);
                close(vizinhos[i].fd);
                vizinhos[i].fd = -1;
                strcpy(vizinhos[i].id, "?");
                continue;
            }

            buffer[n] = '\0';
            char cmd[20] = "";
            sscanf(buffer, "%s", cmd);  /* Extrai o tipo de mensagem */

            /* --- NEIGHBOR <id> : apresentação do vizinho ---
             * Chegou do nó que se ligou a nós (ligação passiva).
             * Atualiza o id='?' com o identificador real.
             * Aproveita para enviar todas as rotas conhecidas ao novo
             * vizinho para ele poder atualizar a sua tabela. */
            if (strcmp(cmd, "NEIGHBOR") == 0) {
                char val[20]; sscanf(buffer, "%*s %s", val);
                strcpy(vizinhos[i].id, val);
                printf("\n[TCP] O vizinho do Slot %d apresentou-se! É o Nó %s.\n", i, val);

                /* Envia ao novo vizinho todas as rotas estáveis que conhecemos.
                 * Só envia rotas em estado EXPEDIÇÃO (state=0) e com rota válida
                 * (dist != -1) para não propagar informação em coordenação. */
                for (int d = 0; d < 100; d++) {
                    if (tabela_rotas[d].state == 0 && tabela_rotas[d].dist != -1) {
                        char rmsg[128];
                        sprintf(rmsg, "ROUTE %02d %d\n", d, tabela_rotas[d].dist);
                        write(vizinhos[i].fd, rmsg, strlen(rmsg));
                    }
                }
            }

            /* --- ROUTE <dest> <dist> : anúncio de rota ---
             * O vizinho i diz que consegue alcançar <dest> em <dist> saltos.
             * A nossa distância seria dist+1 (mais um salto — o que nos liga a i).
             * Só atualizamos se for uma rota melhor que a atual. */
            else if (strcmp(cmd, "ROUTE") == 0) {
                char dest[3]; int dist;
                if (sscanf(buffer, "ROUTE %s %d", dest, &dist) == 2) {
                    int d = atoi(dest);

                    if (tabela_rotas[d].dist == -1 || dist + 1 < tabela_rotas[d].dist) {
                        /* Rota melhor encontrada: atualiza distância e sucessor. */
                        tabela_rotas[d].dist = dist + 1;
                        strcpy(tabela_rotas[d].succ, vizinhos[i].id);

                        /* Propaga a nova rota a todos os vizinhos, mas apenas se
                         * estamos em EXPEDIÇÃO. Em coordenação não propagamos para
                         * evitar criar ciclos durante a renegociação. */
                        if (tabela_rotas[d].state == 0) {
                            char msg[128];
                            sprintf(msg, "ROUTE %02d %d\n", d, dist + 1);
                            for (int j = 0; j < MAX_NEIGHBORS; j++) {
                                if (vizinhos[j].fd != -1)
                                    write(vizinhos[j].fd, msg, strlen(msg));
                            }
                        }
                    }
                }
            }

            /* --- COORD <dest> : pedido de coordenação ---
             * O vizinho i perdeu a sua rota para <dest> e quer saber se
             * dependemos dele. Três casos possíveis:
             *
             * Caso A — Já em coordenação (state=1):
             *   Já sabemos que não temos rota. Respondemos UNCOORD imediatamente.
             *
             * Caso B — Em expedição, mas i NÃO é o nosso sucessor:
             *   Não dependemos de i para alcançar <dest>.
             *   Enviamos a nossa rota atual (se existir) e UNCOORD.
             *
             * Caso C — Em expedição e i É o nosso sucessor:
             *   Dependemos de i — a nossa rota também vai ficar inválida.
             *   Entramos em coordenação: enviamos COORD a todos os vizinhos
             *   (incluindo i) e aguardamos as suas respostas. */
            else if (strcmp(cmd, "COORD") == 0) {
                char dest[3];
                if (sscanf(buffer, "COORD %s", dest) == 1) {
                    int d = atoi(dest);

                    if (tabela_rotas[d].state == 1) {
                        /* Caso A: já em coordenação — não temos rota, respondemos UNCOORD. */
                        char msg[128]; sprintf(msg, "UNCOORD %02d\n", d);
                        write(vizinhos[i].fd, msg, strlen(msg));
                    }
                    else if (tabela_rotas[d].state == 0 &&
                             strcmp(vizinhos[i].id, tabela_rotas[d].succ) != 0) {
                        /* Caso B: i não é o sucessor — enviamos rota conhecida + UNCOORD. */
                        if (tabela_rotas[d].dist != -1) {
                            char rmsg[128];
                            sprintf(rmsg, "ROUTE %02d %d\n", d, tabela_rotas[d].dist);
                            write(vizinhos[i].fd, rmsg, strlen(rmsg));
                        }
                        char umsg[128]; sprintf(umsg, "UNCOORD %02d\n", d);
                        write(vizinhos[i].fd, umsg, strlen(umsg));
                    }
                    else if (tabela_rotas[d].state == 0 &&
                             strcmp(vizinhos[i].id, tabela_rotas[d].succ) == 0) {
                        /* Caso C: i é o nosso sucessor — entramos em coordenação. */
                        tabela_rotas[d].state = 1;
                        /* succ_coord = id de i: quando terminar a coordenação,
                         * enviamos UNCOORD de volta a i. */
                        strcpy(tabela_rotas[d].succ_coord, tabela_rotas[d].succ);
                        tabela_rotas[d].dist = -1;
                        strcpy(tabela_rotas[d].succ, "?");

                        /* Envia COORD a TODOS os vizinhos (incluindo i)
                         * e marca cada um como "à espera de resposta". */
                        char cmsg[128]; sprintf(cmsg, "COORD %02d\n", d);
                        for (int k = 0; k < MAX_NEIGHBORS; k++) {
                            if (vizinhos[k].fd != -1) {
                                tabela_rotas[d].coord[k] = 1;
                                write(vizinhos[k].fd, cmsg, strlen(cmsg));
                            }
                        }
                        /* Verifica se já pode terminar (ex: sem vizinhos a aguardar). */
                        verificar_fim_coordenacao(d, vizinhos, tabela_rotas);
                    }
                }
            }

            /* --- UNCOORD <dest> : resposta de coordenação ---
             * O vizinho i responde que não depende de nós para alcançar <dest>.
             * Marca coord[i] = 0 e verifica se a coordenação pode terminar. */
            else if (strcmp(cmd, "UNCOORD") == 0) {
                char dest[3];
                if (sscanf(buffer, "UNCOORD %s", dest) == 1) {
                    int d = atoi(dest);
                    if (tabela_rotas[d].state == 1) {
                        tabela_rotas[d].coord[i] = 0;  /* Vizinho i já respondeu */
                        verificar_fim_coordenacao(d, vizinhos, tabela_rotas);
                    }
                }
            }

        } /* fim loop vizinhos */

    } /* fim event loop */

    /* Limpeza final: fecha sockets de infraestrutura.
     * Os sockets dos vizinhos seriam fechados aqui num programa completo,
     * mas o OS liberta todos os fds ao terminar o processo. */
    close(fd_udp);
    close(fd_tcp_listen);
    return 0;
}
