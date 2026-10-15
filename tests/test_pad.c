#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
#include "../src/runtime_cam.c"
#include "../src/runtime_menu.c"
#include "../src/runtime_pad.c"
int runtime_memory_is_mapped(uintptr_t address, uint64_t size) {
    (void)address; (void)size;
    return 1;
}

static int capture;
int bbgpu_overlay_captures_input(void) { return capture; }
void bbgpu_mouse_take(double *dx, double *dy, int *up, int *down) { *dx=*dy=0; *up=*down=0; }
int bbgpu_mouse_captured(void) { return 0; }
void bbgpu_mouse_look_enable(int enabled) { (void)enabled; }
void bbgpu_mouse_ui(float *x, float *y, int *left, int *right, int *wheel, int *typing) {
    if (x) *x = 0;
    if (y) *y = 0;
    if (left) *left = 0;
    if (right) *right = 0;
    if (wheel) *wheel = 0;
    if (typing) *typing = 0;
}
void bbgpu_mouse_set_menu(int on) { (void)on; }
void bbgpu_mouse_auto_look(int on) { (void)on; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

static void inject(const char *path, const char *tokens) {
    FILE *f=fopen(path,"w");
    assert(f);
    fputs(tokens,f);
    fclose(f);
    usleep(25000);
}

int main(void) {
    char path[]="/tmp/bbport-pad-test-XXXXXX";
    int fd=mkstemp(path);
    assert(fd>=0);
    close(fd);
    setenv("BB_PAD_FILE",path,1);
    /* bbport.ini controls: buttons moved, a trigger as a button and a button as a trigger. */
    char config[]="/tmp/bbport-pad-config-XXXXXX";
    int config_fd=mkstemp(config);
    assert(config_fd>=0);
    const char controls[]="upscaler=fsr3\npad.cross=b\npad.circle=a\npad.r2=rightshoulder\n"
                          "pad.r1=righttrigger\nkey.cross=X, Space\npad.bogus=a\n";
    assert(write(config_fd,controls,sizeof(controls)-1)==(ssize_t)(sizeof(controls)-1));
    close(config_fd);
    setenv("BB_CONFIG",config,1);
    setenv("SDL_VIDEODRIVER","dummy",1);
    /* Only the virtual test controller is a gamepad, whatever is plugged in. */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0x1d50/0x6189");
    assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD));
    assert(pad_init()==0 && pad_open(1,0,0,NULL)==1);
    PadData data;
    inject(path,"cross l3 touchpad_left");
    assert(pad_read_state(1,&data)==0);
    assert((data.buttons & (BTN_CROSS|BTN_L3|BTN_TOUCHPAD))==(BTN_CROSS|BTN_L3|BTN_TOUCHPAD));
    /* A new touch gets a new id (1..127), as from a DualShock 4: the game ignores id 0. */
    assert(data.touch_count==1 && data.touches[0].x==480 && data.touches[0].y==471 && data.touches[0].id==1);
    inject(path,"touchpad_right");
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.touches[0].x==1440);
    inject(path,"");
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && data.touch_count==0);

    SDL_VirtualJoystickTouchpadDesc touch={.nfingers=2};
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.name="bbport test controller";
    desc.vendor_id=0x1d50;
    desc.product_id=0x6189;
    desc.ntouchpads=1;
    desc.touchpads=&touch;
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);
    assert(id!=0);
    SDL_Joystick *joystick=SDL_OpenJoystick(id);
    assert(joystick);
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,true,0.75f,0.5f,1.0f));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,true,0.25f,1.0f,1.0f));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,true));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0);
    assert(gamepad && data.touch_count==2 && (data.buttons & BTN_TOUCHPAD));
    assert(data.touches[0].x==1439 && data.touches[0].y==471 && data.touches[0].id==2);
    assert(data.touches[1].x==480 && data.touches[1].y==942 && data.touches[1].id==3);
    capture=1;
    assert(pad_read_state(1,&data)==0 && data.touch_count==0 && data.buttons==0);
    capture=0;
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,false,0,0,0));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,false,0,0,0));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.touches[0].x==480);
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,false));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_EAST,true));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,true));
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,32767));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0);
    assert(data.buttons==(BTN_CROSS|BTN_R2|BTN_R1) && data.r2==255);
    assert(bindings[IN_CROSS].key_count==2 && bindings[IN_CROSS].keys[0]==SDL_SCANCODE_X &&
           bindings[IN_CROSS].keys[1]==SDL_SCANCODE_SPACE);
    /* Held right stick, mouse idle: two reads stay equal. The mouse path must not rewrite it. */
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHTX,20000));
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHTY,-20000));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0);
    assert(pad_read_state(1,&data)==0);
    const uint8_t held_x=data.right_x, held_y=data.right_y;
    assert(held_x!=128 && held_y!=128);
    assert(pad_read_state(1,&data)==0 && data.right_x==held_x && data.right_y==held_y);
    /* Losing capture drops a wheel queue instead of replaying it later. */
    mouse_wheel_step(5000000,0,2);
    assert(wheel_phase[1]==1);
    assert(pad_read_state(1,&data)==0);
    assert(wheel_phase[1]==0 && wheel_pending[1]==0 && !mouse_key_down(MOUSE_WHEEL_DOWN));
    SDL_CloseJoystick(joystick);
    if (gamepad) SDL_CloseGamepad(gamepad);
    gamepad=NULL;
    assert(SDL_DetachVirtualJoystick(id));
    SDL_Quit();
    unlink(path);
    unlink(config);
    /* Mouse: names, the look stick's speed-to-deflection at two frame rates, the wheel's presses. */
    assert(key_from_name("Mouse Left")==MOUSE_KEY+SDL_BUTTON_LEFT && key_from_name("wheel down")==MOUSE_WHEEL_DOWN);
    mouse_look_v2=1; mouse_sensitivity=1.0f; mouse_invert_y=0;
    for (int rate=60;rate<=240;rate*=4) {
        PadData m; memset(&m,0,sizeof m);
        look_x=look_y=0; look_last_us=1;
        const uint64_t step=1000000/rate;
        for (int i=1;i<=rate;++i) { /* 500 pixels/s to the right for a second */
            m.right_x=m.right_y=128;
            apply_mouse_look(&m,500.0/rate,0,1+i*step,1);
        }
        assert(m.right_x>=128+16+50 && m.right_x<=128+16+62 && m.right_y==128);
        m.right_x=128;
        apply_mouse_look(&m,0,0,1+(rate+60)*step,1); /* still: back to the pad's stick */
        for (int i=0;i<20;++i) apply_mouse_look(&m,0,0,1+(rate+61+i)*step,1);
        assert(m.right_x==128);
    }
    mouse_wheel_step(1000000,0,2);
    assert(wheel_phase[1]==1 && mouse_key_down(MOUSE_WHEEL_DOWN));
    mouse_wheel_step(1070000,0,0);
    assert(!mouse_key_down(MOUSE_WHEEL_DOWN));
    mouse_wheel_step(1140000,0,0);
    assert(mouse_key_down(MOUSE_WHEEL_DOWN) && wheel_pending[1]==0);
    mouse_wheel_reset();
    assert(wheel_phase[1]==0 && wheel_pending[1]==0 && !mouse_key_down(MOUSE_WHEEL_DOWN));
    /* v2: same speed, same stick, at every poll rate; a hitch is not a spike; stop restores the pad. */
    mouse_look_v2=1; mouse_sensitivity=1.0f; mouse_invert_y=0;
    uint8_t steady=0;
    for (int rate=30;rate<=240;rate*=2) {
        PadData m; memset(&m,0,sizeof m);
        look_x=look_y=0; look_last_us=1;
        const uint64_t step=1000000/rate;
        m.right_x=210; m.right_y=40;
        for (int i=1;i<=rate;++i) {
            m.right_x=210; m.right_y=40;
            apply_mouse_look(&m,500.0/rate,0,1+i*step,1);
        }
        if (!steady) steady=m.right_x;
        assert(m.right_x==steady && m.right_y==128); /* horizontal flick owns Y too */
        m.right_x=210; m.right_y=40;
        apply_mouse_look(&m,0,0,1+(rate+1)*step,1);
        assert(m.right_x==210 && m.right_y==40 && look_x==0 && look_y==0);
    }
    {
        PadData m; memset(&m,0,sizeof m);
        look_x=look_y=0; look_last_us=1000000;
        m.right_x=m.right_y=128;
        apply_mouse_look(&m,125,0,1000000+250000,1); /* 500 px/s across a 250 ms gap */
        assert(m.right_x==steady && m.right_y==128);
        m.right_x=180;
        apply_mouse_look(&m,40,0,1000000+250000,0); /* released: do not turn, clear the filter */
        assert(m.right_x==180 && look_x==0);
    }
    mouse_invert_y=1;
    {
        PadData m; memset(&m,0,sizeof m);
        look_x=look_y=0; look_last_us=1;
        m.right_y=128;
        apply_mouse_look(&m,0,500.0/60,1+1000000/60,1);
        assert(m.right_y<128);
    }
    mouse_invert_y=0; mouse_sensitivity=10.0f;
    {
        PadData m; memset(&m,0,sizeof m);
        look_x=look_y=0; look_last_us=1;
        m.right_x=128;
        apply_mouse_look(&m,100.0/60,0,1+1000000/60,1); /* 100 px/s at sensitivity 10 == full */
        assert(m.right_x==255);
    }
    mouse_sensitivity=1.0f;
    /* One count in a 60 Hz sample still clears the game's small-deflection gap. */
    {
        PadData m; memset(&m,0,sizeof m);
        look_x=look_y=0; look_last_us=1;
        m.right_x=128;
        apply_mouse_look(&m,1,0,1+1000000/60,1);
        assert(m.right_x>=128+16);
    }
    assert(key_axis(200,1,1)==200 && key_axis(128,1,1)==128);
    assert(key_axis(200,1,0)==0 && key_axis(200,0,1)==255 && key_axis(200,0,0)==200);
    /* v1 still coasts, and a held stick survives a still sample. */
    mouse_look_v2=0;
    {
        PadData m; memset(&m,0,sizeof m);
        look_x=look_y=0; look_last_us=1;
        const uint64_t step=1000000/60;
        for (int i=1;i<=60;++i) {
            m.right_x=128;
            apply_mouse_look(&m,500.0/60,0,1+i*step,1);
        }
        assert(m.right_x>=128+16+50 && m.right_x<=128+16+62);
        const uint8_t moving=m.right_x;
        m.right_x=200;
        apply_mouse_look(&m,0,0,1+61*step,1);
        assert(m.right_x!=200 && m.right_x>128); /* coast replaces the pad */
        (void)moving;
    }
    /* Cursor mode: 48 px is one menu step; the larger axis wins. A second notch
     * waits one sample so the menu sees a release between presses. */
    {
        PadData m; memset(&m,0,sizeof m);
        ui_acc_x=ui_acc_y=0; ui_gap=0;
        apply_mouse_cursor(&m,50,10,0,0);
        assert(m.buttons==BTN_RIGHT);
        memset(&m,0,sizeof m);
        ui_acc_x=ui_acc_y=0; ui_gap=0;
        apply_mouse_cursor(&m,0,50,0,0);
        assert(m.buttons==BTN_DOWN);
        memset(&m,0,sizeof m);
        ui_acc_x=ui_acc_y=0; ui_gap=0;
        apply_mouse_cursor(&m,0,0,1,0);
        assert(m.buttons==BTN_UP);
        memset(&m,0,sizeof m);
        ui_acc_x=ui_acc_y=0; ui_gap=0;
        apply_mouse_cursor(&m,100,0,0,0);
        assert(m.buttons==BTN_RIGHT);
        memset(&m,0,sizeof m);
        apply_mouse_cursor(&m,0,0,0,0);
        assert(m.buttons==0);
        memset(&m,0,sizeof m);
        apply_mouse_cursor(&m,0,0,0,0);
        assert(m.buttons==BTN_RIGHT);
    }
    puts("PASS: pad ABI, debug camera chord, left/right clicks, SDL touch coordinates, overlay capture, controls, mouse");
}
