#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Interpreta o comando sem depender de terminador NUL nos buffers MQTT.
 * Retorna 1 para ON, 0 para OFF e -1 para tópico/payload inválido.
 * offset diferente de zero ou total_len diferente de len indica fragmentação:
 * estes comandos curtos são aceitos somente quando chegam completos.
 */
static inline int light_command(const char *topic, size_t topic_len,
                                const char *data, size_t len,
                                size_t total_len, size_t offset)
{
    static const char expected[] = "room1/light";
    if (!topic || !data || offset != 0 || total_len != len ||
        topic_len != sizeof(expected) - 1 ||
        memcmp(topic, expected, sizeof(expected) - 1) != 0) {
        return -1;
    }
    if (len == 2 && memcmp(data, "ON", 2) == 0) return 1;
    if (len == 3 && memcmp(data, "OFF", 3) == 0) return 0;
    return -1;
}

typedef struct {
    bool candidate_pressed; /* Última leitura bruta, ainda em confirmação. */
    bool stable_pressed;    /* Estado confirmado após 40 ms sem mudanças. */
    bool armed;             /* Só permite evento depois de uma soltura estável. */
    int64_t changed_ms;      /* Instante da última mudança da leitura bruta. */
} button_state_t;

/* Começa desarmado: segurar o botão durante o boot não publica um evento. */
static inline button_state_t button_init(bool pressed, int64_t now_ms)
{
    return (button_state_t){
        .candidate_pressed = pressed,
        .stable_pressed = pressed,
        .armed = false,
        .changed_ms = now_ms,
    };
}

/* Máquina de estados de debounce, independente de GPIO e FreeRTOS para
 * permitir testes no computador. now_ms deve vir de um relógio monotônico.
 */
static inline bool button_sample(button_state_t *state, bool pressed, int64_t now_ms)
{
    /* Cada oscilação do contato reinicia a contagem de estabilidade. */
    if (pressed != state->candidate_pressed) {
        state->candidate_pressed = pressed;
        state->changed_ms = now_ms;
    }
    if (now_ms - state->changed_ms < 40) return false;
    /* A soltura confirmada habilita o próximo pressionamento. */
    if (!pressed) {
        state->stable_pressed = false;
        state->armed = true;
        return false;
    }
    /* Não repetir enquanto o botão continuar pressionado. */
    if (state->stable_pressed) return false;
    state->stable_pressed = true;
    bool emit = state->armed;
    state->armed = false;
    return emit;
}
