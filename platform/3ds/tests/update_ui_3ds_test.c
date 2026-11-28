/* Native ROM-backed visual/interaction harness. Captures remain private. */
#include <assert.h>
#include <stdlib.h>
#include "../source/port_second_screen_3ds.c"
static UpdateStatus testStatus = {.state=UPDATE_AVAILABLE,.revision=1,.version="v1.3-E14",.message="UPDATE AVAILABLE"};
static bool testBusy;
static int downloads, checks;
void Updater_GetStatus(UpdateStatus* out) { *out=testStatus; }
bool Updater_Busy(void) { return testBusy; }
void Updater_Cancel(void) { testBusy=false; }
void Updater_Check(void) { ++checks; }
void Updater_Download(void) { ++downloads; }
void Updater_SetChannel(bool pre) { testStatus.prerelease=pre; ++testStatus.revision; }
unsigned Updater_GetNotes(char* out,unsigned cap) {
    snprintf(out,cap,"## Changelog\n\n- Added the Minish Cap updater with stable and experimental channels.\n- Fixed the Goron bottle chest and protected save recovery.\n- Smoothed the bottom-screen Triforce.\n- Numbered diagnostic dumps automatically.\n- Preserved existing settings and controls.\n- Verified downloaded files before installation.");
    return testStatus.revision;
}
void Port_SecondScreen_3DS_LockUI(void) {} void Port_SecondScreen_3DS_UnlockUI(void) {}
static void Capture(const char* name,const uint32_t* pixels,int width,int height,int stride) {
    FILE* f=fopen(name,"wb"); assert(f);
    for(int y=0;y<height;y++) assert(fwrite(pixels+y*stride,4,width,f)==width);
    assert(!fclose(f));
}
int main(int argc,char** argv) {
    assert(argc==2);
    extern void Port_LoadRom(const char*);
    Port_LoadRom(argv[1]);
    assert(Port_SecondScreenTheme_Ready());
    static uint32_t bottom[320*240], paddedBottom[512*256];
    SSurf s={bottom,320,240,320}; TargetList targets={0}; SecondScreenSnapshot snap={0};
    Port_SecondScreenTheme_DrawBackdrop(bottom,320,240,320,0,0,320,240,2);
    PaintSettingsSlab(&s, &snap, &targets, 1.f / 3, 2, SS_SETTINGS_ROOT, 0, 0, 0, 0);
    assert(targets.n == 6);
    assert(targets.t[0].action == SS_ACT_SETTINGS_BACK && targets.t[0].arg == SS_SETTINGS_CLOSE);
    for (int i = 0; i < targets.n; ++i) {
        const TapTarget target = targets.t[i];
        assert(target.x0 >= SETTINGS_SLAB_MARGIN && target.x1 <= 320 - SETTINGS_SLAB_MARGIN);
        assert(target.y0 >= SETTINGS_SLAB_MARGIN && target.y1 <= 240 - SETTINGS_SLAB_MARGIN);
        if (i > 0) {
            assert(target.y1 - target.y0 >= 25);
            assert(target.y0 >= targets.t[i - 1].y1);
        }
    }
    assert(targets.t[5].arg == SS_SETTINGS_UPDATE);
    Capture("settings.raw",bottom,320,240,320);
    sUi.tab=SS_TAB_SETTINGS;sUi.settingsPage=SS_SETTINGS_UPDATE;
    for(int mode=0;mode<5;mode++) {
        targets.n=0;sUpdateNotes=mode==1;sUpdateConfirm=mode==2;
        testStatus.state=mode==3?UPDATE_DOWNLOADING:mode==4?UPDATE_ERROR:UPDATE_AVAILABLE;
        testStatus.progress=57;testStatus.revision++;
        if(mode==4)strcpy(testStatus.message,"CONNECTION FAILED - RETRY");
        PaintSettingsSlab(&s, &snap, &targets, 1.f / 3, 2, SS_SETTINGS_UPDATE, 0, 0, 0, 0);
        for(int i=0;i<targets.n;i++)for(int j=i+1;j<targets.n;j++) {
            TapTarget a=targets.t[i],b=targets.t[j];
            assert(a.x1<=b.x0 || b.x1<=a.x0 || a.y1<=b.y0 || b.y1<=a.y0);
        }
        char file[50];snprintf(file,sizeof(file),"update-%d.raw",mode);Capture(file,bottom,320,240,320);
        for (unsigned i = 0; i < 512 * 256; ++i) paddedBottom[i] = 0x13579bdf;
        SSurf padded = {paddedBottom, 320, 240, 512};
        TargetList paddedTargets = {0};
        PaintSettingsSlab(&padded, &snap, &paddedTargets, 1.f / 3, 2, SS_SETTINGS_UPDATE, 0, 0, 0, 0);
        for (unsigned y = 0; y < 256; ++y)
            for (unsigned x = 0; x < 512; ++x) {
                if (x >= 320 || y >= 240) assert(paddedBottom[y * 512 + x] == 0x13579bdf);
            }
        if (mode == 1) {
            assert(sUpdatePages > 1 && sUpdatePage == 0);
            bool next = false;
            for (int i = 0; i < targets.n; ++i) {
                next |= targets.t[i].action == SS_ACT_UPDATE_NEXT;
                assert(targets.t[i].action != SS_ACT_UPDATE_ACTION);
            }
            assert(next && HandleUpdateTap(SS_ACT_UPDATE_NEXT, 0) && sUpdatePage == 1);
            targets.n = 0;
            PaintSettingsSlab(&s, &snap, &targets, 1.f / 3, 2, SS_SETTINGS_UPDATE, 0, 0, 0, 0);
            bool prev = false;
            for (int i = 0; i < targets.n; ++i)
                prev |= targets.t[i].action == SS_ACT_UPDATE_PREV;
            assert(prev);
            Capture("update-notes-2.raw", bottom, 320, 240, 320);
            assert(HandleUpdateTap(SS_ACT_SETTINGS_BACK, 0) && !sUpdateNotes);
        }
    }
    testStatus.state=UPDATE_AVAILABLE;UpdateUI_Reset();
    assert(HandleUpdateTap(SS_ACT_UPDATE_ACTION,0)&&downloads==0&&sUpdateConfirm);
    assert(HandleUpdateTap(SS_ACT_SETTINGS_BACK,0)&&downloads==0&&!sUpdateConfirm);
    HandleUpdateTap(SS_ACT_UPDATE_ACTION,0); HandleUpdateTap(SS_ACT_UPDATE_ACTION,0);assert(downloads==1);
    testBusy=true;assert(HandleUpdateTap(SS_ACT_TAB,0));assert(sUi.settingsPage==SS_SETTINGS_UPDATE);
    testBusy=false;HandleUpdateTap(SS_ACT_UPDATE_CHANNEL,0);assert(testStatus.prerelease);
    puts("PASS: native Minish Cap theme, five settings rows, touch bounds, changelog, confirmation, busy guard and channels");
}
bool Platform3DS_IsNew3DS(void){return false;}
uint32_t Platform3DS_Core1TimeLimit(void){return 30;}
const char* Port_Config_Get3DSAspectRatioName(void){return "WIDE";}
const char* Port_Config_Get3DSDisplayStyleName(void){return "BILINEAR";}
int Port_Config_Get3DSDisplayStyle(void){return PORT_3DS_DISPLAY_BILINEAR;}
unsigned Port_Config_GetTurboMultiplier(void){return 5;}
double Port_PPU_3DS_CurrentFps(void){return 60;}
double Port_PPU_3DS_AverageFps(void){return 60;}
const char* Port_DumpState_ResultLabel(PortDumpStateResult result){return "LOADED";}
