/* ============================================================
 * MÓDULO DE REDE — rede.c
 * Projeto OWR (OverlayWithRouting)
 *
 * Implementação de todas as funções de gestão de vizinhos
 * e da tabela de encaminhamento declaradas em rede.h.
 *
 * Organização:
 *   1. Funções de gestão de vizinhos
 *        inicializar_vizinhos()
 *        mostrar_vizinhos()
 *        remover_aresta()
 *        aceitar_ligacao()
 *        ligar_vizinho()
 *   2. Funções de gestão de rotas
 *        inicializar_rotas()
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "rede.h"


/* ============================================================
 * 1. GESTÃO DE VIZINHOS
 * ============================================================ */

/* ------------------------------------------------------------
 * inicializar_vizinhos
 *
 * Prepara o array de vizinhos antes de qualquer ligação.
 * Deve ser chamada UMA VEZ no arranque do programa.
 *
 * Sem esta inicialização, os campos fd teriam valores
 * indeterminados (lixo de memória), o que poderia fazer com
 * que o programa tentasse fechar sockets que nunca existiram.
 *
 * Parâmetros:
 *   vizinhos[] — array de MAX_NEIGHBORS slots a inicializar.
 * ------------------------------------------------------------ */
void inicializar_vizinhos(struct Vizinho vizinhos[]) {
    for (int i = 0; i < MAX_NEIGHBORS; i++) {
        /* fd = -1 é a convenção Unix para "sem socket válido".
         * Qualquer slot com fd != -1 tem uma ligação TCP ativa. */
        vizinhos[i].fd = -1;

        /* '?' indica que não há vizinho identificado neste slot. */
        strcpy(vizinhos[i].id, "?");
    }
}


/* ------------------------------------------------------------
 * mostrar_vizinhos
 *
 * Imprime no ecrã todos os vizinhos atualmente conectados.
 * Corresponde ao comando "show neighbors" da interface.
 *
 * Percorre o array completo e imprime apenas os slots com
 * fd != -1 (ligação TCP ativa). Se não houver nenhum,
 * informa o utilizador explicitamente.
 *
 * Parâmetros:
 *   vizinhos[] — array de vizinhos a listar.
 * ------------------------------------------------------------ */
void mostrar_vizinhos(struct Vizinho vizinhos[]) {
    printf("--- Os teus Vizinhos Atuais ---\n");
    int found = 0;

    for (int i = 0; i < MAX_NEIGHBORS; i++) {
        /* Só imprime slots com ligação TCP ativa (fd != -1). */
        if (vizinhos[i].fd != -1) {
            printf("-> Nó %s (no Slot %d)\n", vizinhos[i].id, i);
            found++;
        }
    }

    /* Caso especial: sem vizinhos conectados. */
    if (found == 0) printf("Ainda não tens vizinhos.\n");

    printf("-------------------------------\n");
}


/* ------------------------------------------------------------
 * remover_aresta
 *
 * Remove a ligação TCP com o vizinho identificado por target_id.
 * Corresponde ao comando "remove edge" da interface.
 *
 * Após esta chamada, o slot fica livre para uma nova ligação.
 * IMPORTANTE: o protocolo de encaminhamento (em main.c) deve
 * ser notificado separadamente para recalcular as rotas que
 * passavam por este vizinho.
 *
 * Parâmetros:
 *   vizinhos[]  — array de vizinhos.
 *   target_id   — ID do vizinho a remover (ex: "15").
 * ------------------------------------------------------------ */
void remover_aresta(struct Vizinho vizinhos[], char *target_id) {
    int found = 0;

    for (int i = 0; i < MAX_NEIGHBORS; i++) {
        /* Verifica: slot ocupado E ID corresponde ao alvo. */
        if (vizinhos[i].fd != -1 && strcmp(vizinhos[i].id, target_id) == 0) {
            printf("[TCP] A cortar o cabo com o Nó %s...\n", target_id);

            /* Fecha o socket TCP ao nível do sistema operativo.
             * Isto envia um FIN ao outro lado, sinalizando o fim
             * da ligação. */
            close(vizinhos[i].fd);

            /* Liberta o slot: marca como livre e limpa o ID. */
            vizinhos[i].fd = -1;
            strcpy(vizinhos[i].id, "?");

            found = 1;
            printf("Aresta removida com sucesso!\n");

            /* Não há duplicados: um ID aparece no máximo uma vez.
             * Podemos parar de procurar. */
            break;
        }
    }

    if (!found) {
        printf("Erro: O Nó %s não é teu vizinho atual.\n", target_id);
    }
}


/* ------------------------------------------------------------
 * aceitar_ligacao
 *
 * Aceita uma ligação TCP de entrada — chamada quando há
 * atividade no socket de escuta (fd_tcp_listen).
 *
 * FLUXO:
 *   1. accept() devolve um novo fd dedicado a esta ligação.
 *      O fd_tcp_listen continua disponível para futuras ligações.
 *   2. Procura um slot livre no array de vizinhos.
 *   3a. Se houver espaço: guarda o fd. O id fica '?' porque
 *       ainda não sabemos quem é — aguardamos a mensagem
 *       "NEIGHBOR <id>" que chegará de seguida.
 *   3b. Se não houver espaço: fecha o fd imediatamente para
 *       não deixar o socket pendurado (resource leak).
 *
 * Parâmetros:
 *   fd_tcp_listen — socket de escuta do servidor TCP.
 *   vizinhos[]    — array de vizinhos onde guardar a ligação.
 * ------------------------------------------------------------ */
void aceitar_ligacao(int fd_tcp_listen, struct Vizinho vizinhos[]) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    /* accept() bloqueia até chegar uma ligação TCP.
     * Devolve um novo fd exclusivo para esta ligação. */
    int new_fd = accept(fd_tcp_listen, (struct sockaddr*)&client_addr, &client_len);

    if (new_fd != -1) {
        /* Procura o primeiro slot disponível. */
        int slot_livre = -1;
        for (int i = 0; i < MAX_NEIGHBORS; i++) {
            if (vizinhos[i].fd == -1) { slot_livre = i; break; }
        }

        if (slot_livre != -1) {
            /* Slot encontrado: guarda o fd e aguarda identificação. */
            vizinhos[slot_livre].fd = new_fd;

            /* O ID fica '?' temporariamente. Será atualizado quando
             * chegar a mensagem "NEIGHBOR <id>" do outro nó. */
            strcpy(vizinhos[slot_livre].id, "?");

            printf("\n[TCP] Nova ligação aceite no Slot %d. À espera que se apresente...\n", slot_livre);
        } else {
            /* Array cheio: recusa a ligação imediatamente.
             * close() garante que o socket não fica aberto sem dono. */
            printf("\n[TCP] Rejeitado. Sem espaço para mais vizinhos.\n");
            close(new_fd);
        }
    } else {
        perror("Erro no accept");
    }
}


/* ------------------------------------------------------------
 * ligar_vizinho
 *
 * Estabelece ativamente uma ligação TCP a outro nó e
 * apresenta-se com "NEIGHBOR <meu_id>".
 * Operação inversa de aceitar_ligacao — somos nós a tomar
 * a iniciativa. Usada pelo comando "add edge".
 *
 * FLUXO:
 *   1. Cria um socket TCP com socket().
 *   2. Preenche sockaddr_in com IP e porto do destino.
 *   3. Tenta connect(). Se falhar: fecha fd e reporta erro.
 *   4. Se bem-sucedido: guarda fd, id, ip, tcp no slot livre.
 *   5. Envia imediatamente "NEIGHBOR <meu_id>\n" para que o
 *      outro nó saiba quem somos (e atualize o seu id='?').
 *
 * Parâmetros:
 *   vizinhos[] — array de vizinhos onde guardar a ligação.
 *   res_id     — ID do nó destino (ex: "15").
 *   res_ip     — IP do nó destino (ex: "192.168.1.5").
 *   res_tcp    — Porto TCP do nó destino.
 *   meu_id     — ID deste nó, para enviar na mensagem NEIGHBOR.
 * ------------------------------------------------------------ */
void ligar_vizinho(struct Vizinho vizinhos[], char *res_id,
                   char *res_ip, int res_tcp, char *meu_id) {

    /* Cria socket TCP (SOCK_STREAM = orientado à ligação,
     * com garantias de entrega e ordem). */
    int fd_novo = socket(AF_INET, SOCK_STREAM, 0);

    /* Preenche a estrutura com o endereço do vizinho destino. */
    struct sockaddr_in viz_addr;
    memset(&viz_addr, 0, sizeof(viz_addr));
    viz_addr.sin_family = AF_INET;
    viz_addr.sin_port   = htons(res_tcp);              /* htons: converte para network byte order */
    inet_pton(AF_INET, res_ip, &viz_addr.sin_addr);    /* converte string IP para binário */

    if (connect(fd_novo, (struct sockaddr*)&viz_addr, sizeof(viz_addr)) == -1) {
        /* Falha na ligação: fecha o fd para não criar um socket leak. */
        perror("Erro a ligar");
        close(fd_novo);
    } else {
        /* Ligação TCP estabelecida. Procura um slot livre. */
        int slot_livre = -1;
        for (int i = 0; i < MAX_NEIGHBORS; i++) {
            if (vizinhos[i].fd == -1) { slot_livre = i; break; }
        }

        if (slot_livre != -1) {
            /* Guarda todos os dados do novo vizinho no slot. */
            vizinhos[slot_livre].fd  = fd_novo;
            strcpy(vizinhos[slot_livre].id, res_id);
            strcpy(vizinhos[slot_livre].ip, res_ip);
            vizinhos[slot_livre].tcp = res_tcp;

            printf("[TCP] Ligado com sucesso ao Nó %s!\n", res_id);

            /* Apresentação imediata: envia "NEIGHBOR <meu_id>".
             * O outro nó estava à espera disto para atualizar
             * o seu slot de id='?' com o nosso identificador real. */
            char neighbor_msg[128];
            sprintf(neighbor_msg, "NEIGHBOR %s\n", meu_id);
            write(fd_novo, neighbor_msg, strlen(neighbor_msg));
        } else {
            /* Sem espaço: descarta a ligação já estabelecida.
             * Não há outra opção sem violar o limite MAX_NEIGHBORS. */
            printf("[TCP] Estamos cheios! Impossível adicionar vizinho.\n");
            close(fd_novo);
        }
    }
}


/* ============================================================
 * 2. GESTÃO DA TABELA DE ENCAMINHAMENTO
 * ============================================================ */

/* ------------------------------------------------------------
 * inicializar_rotas
 *
 * Prepara a tabela de encaminhamento antes do início do
 * protocolo. Coloca todas as entradas num estado "desconhecido":
 * sem rota, sem sucessor, no estado de expedição.
 *
 * A tabela tem 100 entradas porque os IDs de nó são de 2
 * dígitos (00–99). O índice corresponde ao ID numérico do
 * destino: tabela_rotas[15] é a rota para o nó "15".
 *
 * Estado inicial de cada entrada:
 *   dist       = -1  → destino inacessível / desconhecido
 *   succ       = '?' → sem vizinho de expedição
 *   state      =  0  → EXPEDIÇÃO (estado estável por omissão)
 *   succ_coord = '?' → sem coordenação ativa
 *   coord[j]   =  0  → sem respostas pendentes
 *
 * Parâmetros:
 *   tabela_rotas[] — array de 100 entradas a inicializar.
 * ------------------------------------------------------------ */
void inicializar_rotas(struct Rota tabela_rotas[]) {
    for (int i = 0; i < 100; i++) {
        /* -1 significa "não sei chegar a este destino". */
        tabela_rotas[i].dist = -1;

        /* '?' = nenhum vizinho de expedição definido ainda. */
        strcpy(tabela_rotas[i].succ, "?");

        /* Estado inicial: EXPEDIÇÃO (0). A coordenação (1) só
         * ocorre quando o nó de expedição falha e é necessário
         * renegociar a rota sem criar ciclos. */
        tabela_rotas[i].state = 0;

        /* '?' = nenhuma coordenação ativa. Será preenchido com o
         * ID do vizinho que desencadeou a entrada em coordenação,
         * para lhe enviar UNCOORD quando a coordenação terminar. */
        strcpy(tabela_rotas[i].succ_coord, "?");

        /* Zera todos os flags de coordenação por vizinho.
         * Durante coordenação, coord[j]=1 significa que ainda
         * aguardamos resposta (UNCOORD ou ROUTE) do vizinho j. */
        for (int j = 0; j < MAX_NEIGHBORS; j++) {
            tabela_rotas[i].coord[j] = 0;
        }
    }
}
