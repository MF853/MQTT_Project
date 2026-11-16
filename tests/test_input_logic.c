#include <assert.h>
#include <stdio.h>
#include "input_logic.h"

int main(void)
{
    const char topic[] = {'r','o','o','m','1','/','l','i','g','h','t'};
    const char on[] = {'O','N'};
    assert(light_command(topic, sizeof(topic), on, sizeof(on), 2, 0) == 1);
    assert(light_command(topic, sizeof(topic), "OFF", 3, 3, 0) == 0);
    assert(light_command(topic, sizeof(topic), "on", 2, 2, 0) == -1);
    assert(light_command(topic, sizeof(topic), "ON\n", 3, 3, 0) == -1);
    assert(light_command(topic, sizeof(topic), "ON\0", 3, 3, 0) == -1);
    assert(light_command(topic, sizeof(topic), "", 0, 0, 0) == -1);
    assert(light_command("room1/switch", 12, on, 2, 2, 0) == -1);
    assert(light_command(topic, sizeof(topic), on, 2, 4, 0) == -1);
    assert(light_command(NULL, 0, on, 2, 4, 2) == -1);

    button_state_t b = button_init(false, 0);
    assert(!button_sample(&b, false, 40));
    assert(!button_sample(&b, true, 50));
    assert(!button_sample(&b, false, 60)); // contact bounce
    assert(!button_sample(&b, true, 70));
    assert(!button_sample(&b, true, 109));
    assert(button_sample(&b, true, 110));
    assert(!button_sample(&b, true, 1000)); // held
    assert(!button_sample(&b, false, 1010));
    assert(!button_sample(&b, true, 1020)); // release bounce
    assert(!button_sample(&b, true, 1060));
    assert(!button_sample(&b, false, 1070));
    assert(!button_sample(&b, false, 1110));
    assert(!button_sample(&b, true, 1120));
    assert(button_sample(&b, true, 1160));

    b = button_init(true, 0); // held during boot
    assert(!button_sample(&b, true, 1000));
    assert(!button_sample(&b, false, 1010));
    assert(!button_sample(&b, false, 1050));
    assert(!button_sample(&b, true, 1060));
    assert(button_sample(&b, true, 1100));
    puts("All input logic tests passed");
}
