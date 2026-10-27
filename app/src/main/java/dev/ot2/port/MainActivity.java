package dev.ot2.port;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import java.lang.ref.WeakReference;
import android.os.Bundle;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.graphics.Color;
import android.view.View;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.InputDevice;
import android.hardware.input.InputManager;
import android.media.AudioDeviceInfo;
import android.media.AudioManager;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.EditText;
import android.text.InputFilter;
import android.util.AtomicFile;
import org.json.JSONObject;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class MainActivity extends Activity implements InputManager.InputDeviceListener {
    static { loadNativeHost(); }
    // MagicOS gates every sideload behind a security check and a lock-screen
    // prompt, which makes a per-build APK install painful while the native host
    // is still being brought up. A copy of the library placed in the app's own
    // files directory takes precedence, so iterating on native code needs only
    // a file push. A normal build has no such file and loads the packaged one.
    //
    // 一个库覆盖机制必须有退出条件：装上新版 APK 之后，files/native 里那份仍然是
    // 上一版（补丁时代推上去的）native 代码，跑起来就是"新版本号 + 旧引擎"。音频这种
    // 只在某一版里才补完整的子系统最容易这样丢——引擎照常往 AAudio 写满帧，日志看着
    // 一切正常，用户那头却是整局没声音。戳文件记下推库时的 versionCode，对不上就
    // 不认这份库（并尽量删掉），让打包库接管。
    private static final String HOT_STAMP="native/libot2host.version";
    private static long installedVersionCode(Context context){
        try{return context.getPackageManager().getPackageInfo(context.getPackageName(),0).getLongVersionCode();}
        catch(Throwable unavailable){return -1;}
    }
    private static boolean hotLibraryMatches(Context context,long version){
        try{
            File stamp=new File(context.getFilesDir(),HOT_STAMP);
            if(!stamp.isFile()||version<0)return false;
            String text=new String(java.nio.file.Files.readAllBytes(stamp.toPath()),StandardCharsets.UTF_8).trim();
            return text.equals(Long.toString(version));
        }catch(Throwable unreadable){return false;}
    }
    private static void loadNativeHost() {
        try {
            Class<?> activityThread = Class.forName("android.app.ActivityThread");
            Object application = activityThread.getMethod("currentApplication").invoke(null);
            if (application instanceof Context) {
                Context context=(Context)application;
                File hot = new File(context.getFilesDir(), "native/libot2host.so");
                if (hot.isFile() && hot.length() > 0) {
                    if (hotLibraryMatches(context,installedVersionCode(context))) {
                        // Android 14+ only allows dynamically loaded code from a
                        // file the app cannot modify afterwards.
                        if (!hot.setReadOnly()) hot.setWritable(false, false);
                        System.load(hot.getAbsolutePath());
                        android.util.Log.i("OT2Native", "loaded " + hot.getAbsolutePath() + " (" + hot.length() + " bytes)");
                        return;
                    }
                    // A stale copy wins the next start too unless it is gone. Deleting
                    // can fail on a file made read-only by an older build; the stamp
                    // check still keeps it out of the way.
                    android.util.Log.w("OT2Native","stale hot library ignored (built for another version) removed="
                        +hot.delete()+": "+hot.getAbsolutePath());
                }
            }
        } catch (Throwable error) {
            android.util.Log.w("OT2Native", "hot library rejected: " + error);
        }
        System.loadLibrary("ot2host");
        android.util.Log.i("OT2Native", "loaded packaged libot2host.so");
    }
    private static native String runNativeProbe(String directory,String privateDirectory,boolean inspectModules);
    private static native void bootNative(String directory,String logPath,long programId);
    private static native void setNativeSurface(Surface surface);
    private static native long getNativeFrameCount();
    private static native double[] getNativeFrameStats();
    private static native int setNativeTargetFps(int target);
    private Surface frameRateSurface;
    private int hintedFrameRate;
    private int applyTargetFps(int target){
        int applied=setNativeTargetFps(target);
        synchronized(surfaceLock){
            Surface current=activeHolder==null?null:activeHolder.getSurface();
            int hint=new File(getFilesDir(),"shaders/no-surface-frame-rate").exists()?0:target;
            if(current!=null&&current.isValid()&&(current!=frameRateSurface||hintedFrameRate!=hint)){
                try{
                    current.setFrameRate(hint,Surface.FRAME_RATE_COMPATIBILITY_DEFAULT,Surface.CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);
                    frameRateSurface=current;hintedFrameRate=hint;
                }catch(IllegalArgumentException|IllegalStateException unavailable){
                    // Surface replacement can race the compositor. Retry on
                    // the next HUD refresh; the native pacer remains active.
                    frameRateSurface=null;
                }
            }
        }
        return applied;
    }
    private PerformanceHud performanceHud;
    // OLED burn-in guard, now opt-in. A stuck white frame left on screen for
    // hours is exactly what wears an emissive panel out, so on an OLED panel the
    // picture is worth hiding. A backlit LCD has no such failure mode, and there
    // the guard only gets in the way: the point of watching the device is to see
    // what it draws. It is therefore off unless files/curtain-on exists.
    //
    // An opaque black layer over the whole window keeps every pixel dark while
    // the SurfaceView underneath keeps rendering -- the game loop, the swapchain
    // and the frame pacing are untouched. It is not clickable, so touches fall
    // through to the virtual gamepad and to the game itself.
    private FrameLayout bootRoot;
    private View blackCurtain;
    private void setCurtain(boolean on){
        if(bootRoot==null) return;
        if(on){
            if(blackCurtain!=null) return;
            blackCurtain=new View(this);
            blackCurtain.setBackgroundColor(Color.BLACK);
            blackCurtain.setClickable(false);blackCurtain.setFocusable(false);
            blackCurtain.setOnTouchListener((v,e)->false);
            bootRoot.addView(blackCurtain,new FrameLayout.LayoutParams(-1,-1));
            android.util.Log.i("OT2Curtain","on (double-tap the screen to reveal)");
        }else{
            if(blackCurtain==null) return;
            bootRoot.removeView(blackCurtain);blackCurtain=null;
            android.util.Log.i("OT2Curtain","off (restart the app to restore)");
        }
    }
    // Reveal gesture: a double tap anywhere. Handled in dispatchTouchEvent
    // *before* the children so the first tap can still be forwarded untouched --
    // a clickable overlay would have swallowed every touch meant for the game.
    // Two taps are only treated as the gesture while the curtain is up, and
    // while it is up the picture is hidden anyway, so a misfire costs nothing.
    private static final long CURTAIN_TAP_MS=420;
    private long curtainTapAt;
    @Override public boolean dispatchTouchEvent(MotionEvent event){
        if(blackCurtain!=null && event.getActionMasked()==MotionEvent.ACTION_DOWN){
            long now=android.os.SystemClock.elapsedRealtime();
            if(now-curtainTapAt<=CURTAIN_TAP_MS){curtainTapAt=0;setCurtain(false);return true;}
            curtainTapAt=now;
        }
        return super.dispatchTouchEvent(event);
    }
    private static native void setNativeFocus(int state);
    private static native int takeNativeSaveNotice();
    private static native String[] getNativeTextInput();
    private static native boolean completeNativeTextInput(long id,boolean accepted,String text);
    interface TextInputBridge {String[] pending();boolean complete(long id,boolean accepted,String text);}
    TextInputBridge textInputBridge=new TextInputBridge(){
        @Override public String[] pending(){return getNativeTextInput();}
        @Override public boolean complete(long id,boolean accepted,String text){return completeNativeTextInput(id,accepted,text);}
    };
    private final Handler noticeHandler=new Handler(Looper.getMainLooper());
    private AlertDialog saveDialog;
    private AlertDialog textDialog;
    private final Runnable noticePoll=new Runnable(){@Override public void run(){
        if(!resumed || isDestroyed())return;
        refreshAutomationInput();
        acceptSaveNotice(takeNativeSaveNotice());
        acceptTextInput(textInputBridge.pending());
        noticeHandler.postDelayed(this,500);
    }};
    void acceptTextInput(String[] request){
        if(request==null){if(textDialog!=null)textDialog.dismiss();return;}
        if(textDialog!=null || saveDialog!=null || !resumed || isFinishing())return;
        long id=Long.parseLong(request[0]);int minimum=Integer.parseInt(request[5]),maximum=Integer.parseInt(request[6]);
        EditText text=new EditText(this);text.setSingleLine(true);text.setFilters(new InputFilter[]{new InputFilter.LengthFilter(maximum)});
        text.setText(request[4]);text.setSelection(text.length());text.setHint(request[3]);
        textDialog=new AlertDialog.Builder(this).setTitle(request[1].isEmpty()?"输入文字":request[1]).setMessage(request[2])
            .setView(text).setPositiveButton("确定",null).setNegativeButton("取消",(dialog,which)->textInputBridge.complete(id,false,""))
            .setOnCancelListener(dialog->textInputBridge.complete(id,false,"")).create();
        textDialog.setOnDismissListener(dialog->textDialog=null);
        textDialog.setOnKeyListener((dialog,code,event)->{
            if(code!=KeyEvent.KEYCODE_BUTTON_A&&code!=KeyEvent.KEYCODE_BUTTON_B)return false;
            if(event.getAction()==KeyEvent.ACTION_UP){if(code==KeyEvent.KEYCODE_BUTTON_B){textInputBridge.complete(id,false,"");dialog.dismiss();}else textDialog.getButton(AlertDialog.BUTTON_POSITIVE).performClick();}return true;
        });
        textDialog.show();textDialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(view->{
            String value=text.getText().toString();if(value.length()<minimum){text.setError("至少输入 "+minimum+" 个字符");return;}
            if(textInputBridge.complete(id,true,value))textDialog.dismiss();
        });
        text.requestFocus();textDialog.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
    }
    // Main-thread state. Session keeps unacknowledged notices through Activity
    // replacement; no native callback owns an Activity or waits for the UI.
    void acceptSaveNotice(int flags){
        Session.pendingSaveNotice|=flags;
        int pending=Session.pendingSaveNotice;
        if(pending==0 || !resumed || isFinishing() || isDestroyed())return;
        String message=(pending&4)!=0
            ?"存档写入后未能确认已安全保存。请检查设备剩余空间并再次保存；在确认成功前，不要关闭游戏。"
            :(pending&1)!=0
            ?"存档目前只能读取，本次游玩的新进度无法保存。请检查设备剩余空间，重新启动游戏后再试。已保存的进度未被修改。"
            :"本次保存失败，之前已保存的进度未被修改。请检查设备剩余空间后再次保存。";
        if((pending&5)==5)message+="\n当前存档也只能读取，无法再次保存。";
        if(saveDialog!=null){saveDialog.setMessage(message);return;}
        saveDialog=new AlertDialog.Builder(this).setTitle("存档提示").setMessage(message)
            .setCancelable(false).setPositiveButton("知道了",(dialog,which)->Session.pendingSaveNotice=0).create();
        saveDialog.setOnDismissListener(dialog->saveDialog=null);
        saveDialog.setOnKeyListener((dialog,code,event)->{
            if(code!=KeyEvent.KEYCODE_BUTTON_A)return false;
            if(event.getAction()==KeyEvent.ACTION_UP)saveDialog.getButton(AlertDialog.BUTTON_POSITIVE).performClick();
            return true;
        });
        saveDialog.show();
    }
    private boolean resumed;
    private boolean bootRequested(){return !getIntent().getBooleanExtra("probe_only",false);}
    private static native void setNativeInputTrace(boolean enabled);
    private boolean inputTrace,automationInput;
    private boolean acceptsInput(int deviceId){return !automationInput||deviceId==android.view.KeyCharacterMap.VIRTUAL_KEYBOARD;}
    private void refreshAutomationInput(){
        boolean next=new File(getFilesDir(),"automation-input").exists();
        if(next!=automationInput){releaseInput();automationInput=next;if(inputTrace)android.util.Log.i("OT2Input","automation="+next);}
    }
    private static final Object surfaceLock=new Object();
    private static SurfaceHolder activeHolder;
    private TextView status;
    private Button retry;
    private boolean started;
    private static native void setNativePad(long buttons,int lx,int ly,int rx,int ry);
    private static native void clearNativePad();
    private static final ActivitySession activitySession=new ActivitySession(MainActivity::setNativeFocus,MainActivity::clearNativePad);
    private static native void setNativeTouch(int action,int id,float x,float y,int width,int height);
    private static native void cancelNativeTouch();
    private GameInputView gameInput;
    private PadToggleView padToggle;
    // 性能面板默认隐藏：它固定在左上角，正好压住本作自己的倒计时与题面抬头。诊断要一次手势就能拿到，
    // 但默认不能占着画面 —— 所以由 HudToggleView 那个悬浮把手显式打开。
    // The HUD starts hidden: it is pinned to the top-left, which is exactly where this
    // title draws its countdown and question header. It stays one gesture away behind
    // the HudToggleView handle instead of covering the picture by default.
    private HudToggleView hudToggle;
    private boolean hudVisible;
    private long virtualButtons;
    private int virtualLx,virtualLy,virtualRx,virtualRy;
    private long keyButtons,axisButtons;
    private int lx,ly,rx,ry,controllerId=-1;
    private void updateButtons(){if(resumed&&activitySession.owns(this))setNativePad(keyButtons|axisButtons|virtualButtons,lx!=0?lx:virtualLx,ly!=0?ly:virtualLy,rx!=0?rx:virtualRx,ry!=0?ry:virtualRy);}
    private void releaseInput(){if(inputTrace)android.util.Log.i("OT2Input","clear device="+controllerId);keyButtons=axisButtons=0;lx=ly=rx=ry=0;virtualButtons=0;virtualLx=virtualLy=virtualRx=virtualRy=0;if(gameInput!=null)gameInput.cancel();if(activitySession.owns(this))clearNativePad();}
    private static long buttonForKey(int code) {
        switch(code) {
            case KeyEvent.KEYCODE_BUTTON_A: case KeyEvent.KEYCODE_ENTER: case KeyEvent.KEYCODE_Z: return 1;
            case KeyEvent.KEYCODE_BUTTON_B: case KeyEvent.KEYCODE_X: return 2;
            case KeyEvent.KEYCODE_BUTTON_X: return 4;
            case KeyEvent.KEYCODE_BUTTON_Y: return 8;
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return 1L<<4;
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return 1L<<5;
            case KeyEvent.KEYCODE_BUTTON_L1: return 1L<<6;
            case KeyEvent.KEYCODE_BUTTON_R1: return 1L<<7;
            case KeyEvent.KEYCODE_BUTTON_L2: return 1L<<8;
            case KeyEvent.KEYCODE_BUTTON_R2: return 1L<<9;
            case KeyEvent.KEYCODE_BUTTON_START: return 1L<<10;
            case KeyEvent.KEYCODE_BUTTON_SELECT: return 1L<<11;
            case KeyEvent.KEYCODE_DPAD_LEFT: return 1L<<12;
            case KeyEvent.KEYCODE_DPAD_UP: return 1L<<13;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return 1L<<14;
            case KeyEvent.KEYCODE_DPAD_DOWN: return 1L<<15;
            default:return 0;
        }
    }
    @Override public boolean onKeyDown(int code,KeyEvent event) {long bit=buttonForKey(code);if(bit!=0&&!acceptsInput(event.getDeviceId())){if(inputTrace)android.util.Log.i("OT2Input","ignored-key device="+event.getDeviceId()+" code="+code);return true;}if(inputTrace)android.util.Log.i("OT2Input","down code="+code+" mapped="+bit+" device="+event.getDeviceId()+" source="+event.getSource()+" repeat="+event.getRepeatCount()+" time="+event.getEventTime());if(bit!=0){controllerId=event.getDeviceId();keyButtons|=bit;updateButtons();return true;}return super.onKeyDown(code,event);}
    @Override public boolean onKeyUp(int code,KeyEvent event) {long bit=buttonForKey(code);if(bit!=0&&!acceptsInput(event.getDeviceId())){if(inputTrace)android.util.Log.i("OT2Input","ignored-key device="+event.getDeviceId()+" code="+code);return true;}if(inputTrace)android.util.Log.i("OT2Input","up code="+code+" mapped="+bit+" device="+event.getDeviceId()+" source="+event.getSource()+" repeat="+event.getRepeatCount()+" time="+event.getEventTime());if(bit!=0){keyButtons&=~bit;updateButtons();return true;}return super.onKeyUp(code,event);}
    private static float axis(MotionEvent event,int axis) {
        InputDevice device=event.getDevice();
        InputDevice.MotionRange range=device==null?null:device.getMotionRange(axis,event.getSource());
        float value=event.getAxisValue(axis),flat=range==null?0.12f:Math.max(0.12f,range.getFlat());
        return Math.abs(value)<=flat?0:Math.copySign(Math.min(1,(Math.abs(value)-flat)/(1-flat)),value);
    }
    @Override public boolean onGenericMotionEvent(MotionEvent event) {
        if((event.getSource()&InputDevice.SOURCE_JOYSTICK)==InputDevice.SOURCE_JOYSTICK && event.getAction()==MotionEvent.ACTION_MOVE) {
            if(!acceptsInput(event.getDeviceId())){if(inputTrace)android.util.Log.i("OT2Input","ignored-axis device="+event.getDeviceId()+" x="+event.getAxisValue(MotionEvent.AXIS_X)+" y="+event.getAxisValue(MotionEvent.AXIS_Y));return true;}
            long oldButtons=axisButtons;int oldLx=lx,oldLy=ly,oldRx=rx,oldRy=ry;
            controllerId=event.getDeviceId();
            lx=Math.round(axis(event,MotionEvent.AXIS_X)*32767);ly=Math.round(-axis(event,MotionEvent.AXIS_Y)*32767);
            InputDevice device=event.getDevice();
            boolean zr=device!=null && device.getMotionRange(MotionEvent.AXIS_Z,event.getSource())!=null;
            rx=Math.round(axis(event,zr?MotionEvent.AXIS_Z:MotionEvent.AXIS_RX)*32767);
            ry=Math.round(-axis(event,zr?MotionEvent.AXIS_RZ:MotionEvent.AXIS_RY)*32767);
            float hx=event.getAxisValue(MotionEvent.AXIS_HAT_X),hy=event.getAxisValue(MotionEvent.AXIS_HAT_Y);
            axisButtons=(hx<-.5f?1L<<12:0)|(hy<-.5f?1L<<13:0)|(hx>.5f?1L<<14:0)|(hy>.5f?1L<<15:0);
            if(Math.max(event.getAxisValue(MotionEvent.AXIS_LTRIGGER),event.getAxisValue(MotionEvent.AXIS_BRAKE))>.5f)axisButtons|=1L<<8;
            if(Math.max(event.getAxisValue(MotionEvent.AXIS_RTRIGGER),event.getAxisValue(MotionEvent.AXIS_GAS))>.5f)axisButtons|=1L<<9;
            if(inputTrace&&(oldButtons!=axisButtons||oldLx!=lx||oldLy!=ly||oldRx!=rx||oldRy!=ry))android.util.Log.i("OT2Input","axes device="+controllerId+" buttons="+axisButtons+" lx="+lx+" ly="+ly+" rx="+rx+" ry="+ry+" time="+event.getEventTime());
            updateButtons();return true;
        }
        return super.onGenericMotionEvent(event);
    }
    private void refreshController(){
        if(gameInput!=null&&gameInput.setControllerConnected(GameInputView.controllerPresent()))releaseInput();
        syncPadToggle();
    }
    // The toggle shows what the pad is actually doing, not what was last asked for: a
    // connected controller turns the pad off on its own, and a switch that still reads
    // "on" with no pad on screen is a lie the player has no way to correct.
    private void syncPadToggle(){if(padToggle!=null)padToggle.setPadOn(gameInput!=null&&gameInput.controlsVisible());}
    private void placePadToggle(FrameLayout root,PadToggleView toggle){
        final int margin=Math.round(10*getResources().getDisplayMetrics().density);
        root.post(new Runnable(){@Override public void run(){
            if(isDestroyed()||root.getWidth()<=0){if(!isDestroyed())root.postDelayed(this,50);return;}
            android.view.WindowInsets insets=root.getRootWindowInsets();
            android.graphics.Insets safe=insets==null?null:insets.getInsets(android.view.WindowInsets.Type.displayCutout());
            // The title draws 16:9 into a 16:10 surface, so a handle pinned to the very
            // top corner sits half inside the letterbox bar. Start it just below that
            // bar; the player can still drag it anywhere afterwards.
            int letterbox=Math.max(0,(root.getHeight()-Math.round(root.getWidth()*9f/16f))/2);
            FrameLayout.LayoutParams params=(FrameLayout.LayoutParams)toggle.getLayoutParams();
            params.leftMargin=Math.max(0,root.getWidth()-toggle.getWidth()-margin-(safe==null?0:safe.right));
            params.topMargin=letterbox+margin+(safe==null?0:safe.top);
            toggle.setLayoutParams(params);
        }});
    }
    // Directly under the pad handle, same right edge: the two controls sit in one column so
    // neither has to be hunted for, and a player who drags one out of the way does not take
    // the other with it.
    private void placeHudToggle(FrameLayout root,HudToggleView toggle){
        final int margin=Math.round(10*getResources().getDisplayMetrics().density);
        final int gap=Math.round(8*getResources().getDisplayMetrics().density);
        root.post(new Runnable(){@Override public void run(){
            if(isDestroyed()||root.getWidth()<=0){if(!isDestroyed())root.postDelayed(this,50);return;}
            android.view.WindowInsets insets=root.getRootWindowInsets();
            android.graphics.Insets safe=insets==null?null:insets.getInsets(android.view.WindowInsets.Type.displayCutout());
            int letterbox=Math.max(0,(root.getHeight()-Math.round(root.getWidth()*9f/16f))/2);
            FrameLayout.LayoutParams params=(FrameLayout.LayoutParams)toggle.getLayoutParams();
            params.leftMargin=Math.max(0,root.getWidth()-toggle.getWidth()-margin-(safe==null?0:safe.right));
            int top=letterbox+margin+(safe==null?0:safe.top);
            int padHeight=padToggle!=null && padToggle.getHeight()>0?padToggle.getHeight():Math.round(46*getResources().getDisplayMetrics().density);
            params.topMargin=top+padHeight+gap;
            toggle.setLayoutParams(params);
        }});
    }
    @Override public void onInputDeviceAdded(int id) {refreshController();}
    @Override public void onInputDeviceChanged(int id) {if(id==controllerId)releaseInput();refreshController();}
    @Override public void onInputDeviceRemoved(int id) {if(id==controllerId){releaseInput();controllerId=-1;}refreshController();}
    @Override protected void onStart(){super.onStart();activitySession.start(this);}
    @Override protected void onResume(){super.onResume();resumed=true;refreshAutomationInput();getSystemService(InputManager.class).registerInputDeviceListener(this,null);activitySession.resume(this,hasWindowFocus());refreshController();noticeHandler.post(noticePoll);if(performanceHud!=null&&hudVisible)performanceHud.start();}
    @Override public void onWindowFocusChanged(boolean focused){super.onWindowFocusChanged(focused);if(resumed){activitySession.focus(this,focused);if(!focused)releaseInput();}}
    @Override protected void onPause(){resumed=false;noticeHandler.removeCallbacks(noticePoll);if(performanceHud!=null)performanceHud.stop();if(saveDialog!=null)saveDialog.dismiss();if(textDialog!=null)textDialog.dismiss();getSystemService(InputManager.class).unregisterInputDeviceListener(this);activitySession.pause(this);releaseInput();super.onPause();}
    @Override protected void onStop(){activitySession.stop(this);super.onStop();}


    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        inputTrace=getIntent().getBooleanExtra("input_trace",false);setNativeInputTrace(inputTrace);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setBackgroundColor(Color.rgb(20,25,32));
        int pad = (int)(24 * getResources().getDisplayMetrics().density);
        layout.setPadding(pad,pad,pad,pad);
        layout.setOnApplyWindowInsetsListener((view,insets) -> {
            android.graphics.Insets bars=insets.getInsets(android.view.WindowInsets.Type.systemBars());
            view.setPadding(pad+bars.left,pad+bars.top,pad+bars.right,pad+bars.bottom);
            return insets;
        });
        TextView title = new TextView(this);
        title.setText("Switch2Android");
        title.setTextSize(24); title.setTextColor(Color.rgb(236,216,171));
        layout.addView(title);
        TextView note = new TextView(this);
        note.setText("本地游戏 · ARM64 + Vulkan\n无手柄时显示屏幕按键，连接后自动隐藏。\n");
        note.setTextSize(16); note.setTextColor(Color.LTGRAY); layout.addView(note);
        retry = new Button(this); retry.setText("重新运行验证");
        retry.setOnClickListener(v -> probe()); layout.addView(retry);
        status = new TextView(this); status.setTextSize(15); status.setTextColor(Color.WHITE);
        status.setTextIsSelectable(true);
        ScrollView scroll = new ScrollView(this); scroll.addView(status);
        layout.addView(scroll,new LinearLayout.LayoutParams(-1,0,1));
        if(bootRequested()) {
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            FrameLayout root=new FrameLayout(this);
            bootRoot=root;
            SurfaceView surface=new SurfaceView(this);
            // gfx-half-present（默认开启，标记 shaders/no-half-present 关闭）：上屏缓冲按
            // 屏幕的一半分配，2 倍放大交给合成器。现在 guest 出 1280x720，我们要先在 GPU
            // 里放大成 2560x1600，合成器再放大到屏幕上 —— 同一批像素被重采样两次，占掉单帧
            // 预算里最大的一块填充率（实测 GPU 13.0ms 里有 5.5ms 是它）。缓冲减半后本机只写
            // 1/4 的像素，由合成器做整数 2 倍放大，画质不变。屏幕不够大时保持原尺寸。
            android.util.DisplayMetrics metrics=getResources().getDisplayMetrics();
            if(metrics.widthPixels>=2560&&metrics.heightPixels>=1440
                    &&!new File(getFilesDir(),"shaders/no-half-present").exists())
                surface.getHolder().setFixedSize(metrics.widthPixels/2,metrics.heightPixels/2);
            root.addView(surface,new FrameLayout.LayoutParams(-1,-1));
            layout.removeAllViews();scroll.removeView(status);
            layout.setBackgroundColor(Color.TRANSPARENT);status.setTextSize(12);
            layout.addView(status,new LinearLayout.LayoutParams(-1,-2));
            root.addView(layout,new FrameLayout.LayoutParams(-1,-2));
            gameInput=new GameInputView(this,new GameInputView.Sink(){
                @Override public void pad(long buttons,int x,int y,int z,int w){virtualButtons=buttons;virtualLx=x;virtualLy=y;virtualRx=z;virtualRy=w;updateButtons();}
                @Override public void touch(int action,int id,float x,float y,int w,int h){if(resumed&&hasWindowFocus()&&activitySession.owns(MainActivity.this))setNativeTouch(action,id,x,y,w,h);}
                @Override public void cancelTouch(){if(activitySession.owns(MainActivity.this))cancelNativeTouch();}
            });
            root.addView(gameInput,new FrameLayout.LayoutParams(-1,-1));
            performanceHud=new PerformanceHud(this,MainActivity::getNativeFrameStats,this::applyTargetFps);
            FrameLayout.LayoutParams hudParams=new FrameLayout.LayoutParams(-2,-2,android.view.Gravity.TOP|android.view.Gravity.START);
            int margin=Math.round(12*getResources().getDisplayMetrics().density);
            hudParams.setMargins(margin,margin,margin,margin);root.addView(performanceHud,hudParams);
            performanceHud.setVisibility(View.GONE);   // off until the handle asks for it
            performanceHud.setOnApplyWindowInsetsListener((view,insets)->{
                android.graphics.Insets safe=insets.getInsets(android.view.WindowInsets.Type.displayCutout());
                FrameLayout.LayoutParams p=(FrameLayout.LayoutParams)view.getLayoutParams();
                p.setMargins(margin+safe.left,margin+safe.top,margin+safe.right,margin+safe.bottom);view.setLayoutParams(p);return insets;
            });
            // Added last so it sits above the HUD and the pad: the handle has to stay
            // reachable even while the pad it controls is drawn across the whole window.
            padToggle=new PadToggleView(this,enabled->{
                gameInput.setPadEnabled(enabled);
                syncPadToggle();
            });
            root.addView(padToggle,new FrameLayout.LayoutParams(-2,-2,android.view.Gravity.TOP|android.view.Gravity.START));
            placePadToggle(root,padToggle);
            // 第二个悬浮把手，管性能面板的显隐；交互方式与上面那个虚拟按键开关完全一致。
            // A second handle, for the HUD, with exactly the same gesture model as the pad switch above.
            hudToggle=new HudToggleView(this,on->{
                hudVisible=on;
                if(performanceHud!=null){
                    performanceHud.setVisibility(on?View.VISIBLE:View.GONE);
                    if(on)performanceHud.start();else performanceHud.stop();
                }
            });
            root.addView(hudToggle,new FrameLayout.LayoutParams(-2,-2,android.view.Gravity.TOP|android.view.Gravity.START));
            placeHudToggle(root,hudToggle);
            surface.getHolder().addCallback(new SurfaceHolder.Callback() {
                @Override public void surfaceCreated(SurfaceHolder holder) {
                    synchronized(surfaceLock){frameRateSurface=null;activeHolder=holder;setNativeSurface(holder.getSurface());}
                    if(!started){started=true;probe();}
                }
                @Override public void surfaceChanged(SurfaceHolder holder,int format,int width,int height) {}
                @Override public void surfaceDestroyed(SurfaceHolder holder) {synchronized(surfaceLock){if(activeHolder==holder){frameRateSurface=null;activeHolder=null;setNativeSurface(null);}}}
            });
            setContentView(root);
            // Opt-in guard: only a session that asked for it starts dark. The
            // default has to be visible, because a session that sits on a stalled
            // frame and shows nothing is indistinguishable from a session that
            // never drew at all -- and telling those two apart is the whole job.
            if(new File(getFilesDir(),"curtain-on").exists())setCurtain(true);
            root.post(()->{
                getWindow().getInsetsController().hide(android.view.WindowInsets.Type.systemBars());
                getWindow().getInsetsController().setSystemBarsBehavior(android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            });
            root.postDelayed(new Runnable(){@Override public void run(){if(isDestroyed())return;if(getNativeFrameCount()>4){status.setVisibility(View.GONE);return;}root.postDelayed(this,500);}},500);
        } else {setContentView(layout);layout.requestApplyInsets();probe();}
    }

    // The native game owns process-wide state and outlives Activity recreation.
    // Only application Context and weak UI references may cross bootNative().
    private static final class Session {
        enum Phase { IDLE, VALIDATING, RUNNING, ENDED, FAILED }
        private static final ExecutorService worker=Executors.newSingleThreadExecutor();
        private static Phase phase=Phase.IDLE;
        private static boolean gameClaimed,bootRequested;
        private static int pendingSaveNotice;
        private static WeakReference<MainActivity> observer=new WeakReference<>(null);
        private static String message="正在校验并启动游戏…";
        private static void publish(String text){synchronized(Session.class){message=text;notifyObserver();}}
        private static void notifyObserver(){
            MainActivity activity=observer.get();if(activity==null)return;
            String text=message;boolean retryAllowed=!gameClaimed && phase!=Phase.VALIDATING;
            // The posted callback is short-lived; the native worker never owns
            // an Activity while the game runs indefinitely.
            activity.runOnUiThread(()->{if(!activity.isDestroyed()){activity.status.setText(text);activity.retry.setEnabled(retryAllowed);}});
        }
        static synchronized void detach(MainActivity activity){if(observer.get()==activity)observer.clear();}
        static synchronized void request(MainActivity activity,boolean boot){
            observer=new WeakReference<>(activity);
            if(gameClaimed){notifyObserver();return;}
            bootRequested|=boot;
            if(phase==Phase.VALIDATING){notifyObserver();return;}
            phase=Phase.VALIDATING;message="正在校验并启动游戏…";notifyObserver();
            Context context=activity.getApplicationContext();
            String title=activity.getIntent().getStringExtra("title_id");
            worker.execute(()->run(context,title));
        }
        private static void run(Context context,String title){
            try {
                long started=android.os.SystemClock.elapsedRealtime();
                GameProfile profile=GameProfile.select(context,title);
                // d2r 只从私有目录读着色器包，game.s2a 里那份运行时不读，所以首启
                // （含卸载重装）要在这儿把它们铺好，否则一进 compute dispatch 就
                // "Dispatch has no genuine compute shader" 直接退出。
                ShaderBundles.install(context,profile.privateRoot,Session::publish);
                // 管线预热列表的种子：让新设备在第一场对战前就把常见管线建好，
                // 而不是在战斗中边玩边编译。没有列表（全新安装）时才由引擎采用。
                PipelineSeed.install(context,profile.privateRoot,Session::publish);
                // 同理：中文替换字体也只在私有目录里找，系统字体没有中文字形，
                // 缺了就是满屏 "?"。卸载重装会连它一起删，所以也要首启铺回去。
                SharedFont.install(context,profile.privateRoot,Session::publish);
                File directory=new File(profile.gameRoot,"ExeFS");
                String validation=profile.validateModules();
                long verified=android.os.SystemClock.elapsedRealtime();
                boolean inspect; synchronized(Session.class){inspect=!bootRequested;}
                // Boot maps, decompresses and links the modules itself. The
                // explicit probe retains full inspection without doing that
                // same expensive work twice on every normal launch.
                JSONObject report=new JSONObject(runNativeProbe(validation.isEmpty()?directory.getAbsolutePath():"",profile.privateRoot.getAbsolutePath(),inspect));
                report.put("title_id",profile.titleId);
                report.put("module_validation_ms",verified-started);
                report.put("probe_ms",android.os.SystemClock.elapsedRealtime()-verified);
                report.put("model",Build.MODEL);report.put("soc",Build.SOC_MODEL);
                report.put("android_api",Build.VERSION.SDK_INT);
                report.put("module_validation",validation.isEmpty()?"sha256_verified":validation);
                report.put("timestamp_ms",System.currentTimeMillis());
                AtomicFile output=new AtomicFile(new File(profile.privateRoot,"probe.json"));
                FileOutputStream stream=output.startWrite();
                try{stream.write(report.toString(2).getBytes(StandardCharsets.UTF_8));output.finishWrite(stream);}
                catch(Exception error){output.failWrite(stream);throw error;}
                // A diagnostic run exists to enumerate system service demands,
                // not to prove the display path, so the graphics probe results
                // are absent by design and must not block the boot.
                boolean valid=report.optBoolean("graphics_probes_skipped")
                    ? validation.isEmpty() && report.optBoolean("save_atomic_publish")
                        && report.optBoolean("synthetic_arm64_fixture") && (!inspect||report.has("loader"))
                    : validation.isEmpty() && report.has("execution_bridge")
                        && report.optBoolean("save_atomic_publish") && report.optBoolean("synthetic_arm64_fixture") && report.optBoolean("nvn_texture_transfers")
                        && report.optBoolean("nvn_descriptor_reuse") && report.optBoolean("nvn_resource_lifecycle") && report.optBoolean("nvn_texture_cpu_coherence") && report.optBoolean("nvn_gpu_deswizzle")
                        && report.optBoolean("aaudio_disconnect_reopen_injected") && (!inspect||report.has("loader"));
                boolean startGame;
                synchronized(Session.class){
                    startGame=bootRequested && valid;
                    phase=startGame?Phase.RUNNING:Phase.IDLE;
                    if(startGame)gameClaimed=true;
                }
                if(startGame){
                    publish("正在启动游戏…");
                    recordAudioEnvironment(context);
                    bootNative(directory.getAbsolutePath(),new File(profile.privateRoot,"boot.log").getAbsolutePath(),Long.parseUnsignedLong(profile.titleId,16));
                    synchronized(Session.class){phase=Phase.ENDED;}
                    publish("游戏运行已结束。");
                }else{
                    publish(render(report));
                }
            }catch(Exception error){
                synchronized(Session.class){phase=Phase.FAILED;}
                publish("验证失败："+error.getMessage());
            }
        }
    }
    // 手机"没有声音"有一半是设备侧状态：媒体音量为 0、静音/勿扰、声音被路由到蓝牙或
    // 耳机。这些从引擎日志里看不出来（引擎照常往 AAudio 写满 48 kHz 帧），只能问系统。
    // 每次启动前抓一份写进 files/audio-env.txt 和 logcat，下次连 adb 就能一眼分出
    // "引擎没输出" 和 "手机自己在静音"，不必再靠猜。
    private static void recordAudioEnvironment(Context context){
        try{
            AudioManager audio=(AudioManager)context.getSystemService(Context.AUDIO_SERVICE);
            if(audio==null)return;
            StringBuilder outputs=new StringBuilder();
            for(AudioDeviceInfo device:audio.getDevices(AudioManager.GET_DEVICES_OUTPUTS)){
                if(device==null)continue;
                outputs.append(device.getType()).append('|').append(device.getProductName()).append(';');
            }
            String line="media_volume="+audio.getStreamVolume(AudioManager.STREAM_MUSIC)
                +"/"+audio.getStreamMaxVolume(AudioManager.STREAM_MUSIC)
                +" muted="+audio.isStreamMute(AudioManager.STREAM_MUSIC)
                +" ringer="+audio.getRingerMode()
                +" mode="+audio.getMode()
                +" outputs="+(outputs.length()==0?"none":outputs.toString());
            android.util.Log.i("OT2Audio",line);
            try(FileOutputStream out=new FileOutputStream(new File(context.getFilesDir(),"audio-env.txt"))){
                out.write(line.getBytes(StandardCharsets.UTF_8));
            }
        }catch(Throwable ignored){
            // A missing audio service must not stop the boot.
        }
    }
    private void probe(){Session.request(this,bootRequested());}

    private static String render(JSONObject r) {
        StringBuilder text=new StringBuilder();
        text.append(r.optString("model")).append(" · ").append(r.optString("soc")).append('\n');
        text.append("内存页：").append(r.optLong("page_size")).append(" bytes\n\n");
        if(r.has("compatibility_error"))return text.append(r.optString("compatibility_error")).append("\n\n本次未启动游戏。设备检查结果已保存。").toString();
        text.append("ARM64 合成指令测试：").append(r.optBoolean("synthetic_arm64_fixture")?"通过":"失败").append('\n');
        JSONObject gpu=r.optJSONObject("vulkan");
        if(gpu!=null) text.append(gpu.optString("device")).append(" · Vulkan ").append(gpu.optString("api"))
                .append("\nGPU 清屏并回读：通过\nBC 纹理：").append(gpu.optBoolean("texture_bc")?"原生支持":"软件解码").append('\n');
        else text.append("Vulkan：").append(r.optString("vulkan_error")).append('\n');
        text.append("\n游戏模块校验：").append(r.optString("module_validation")).append('\n');
        JSONObject loader=r.optJSONObject("loader");
        if(loader!=null) text.append(loader.optJSONArray("modules").length()).append(" 个模块已装载\n重定位：").append(loader.optLong("relocations")).append(" 处\n");
        if(loader!=null && loader.optJSONObject("sdk_leaf")!=null) text.append("原版 SDK 独立函数已原生执行：")
                .append(loader.optJSONObject("sdk_leaf").optLong("returned_hz")).append(" Hz\n");
        if(r.has("loader_error")) text.append("装载错误：").append(r.optString("loader_error")).append('\n');
        text.append("\nCPU 执行边界：").append(r.has("execution_bridge")?"已验证":"未通过");
        text.append("\n待验证：游戏画面、音频、输入、存档。\n\n");
        text.append("报告保存于应用内部 files/probe.json");
        return text.toString();
    }
    @Override public void onDestroy() { noticeHandler.removeCallbacks(noticePoll);if(saveDialog!=null)saveDialog.dismiss();Session.detach(this);activitySession.destroy(this);super.onDestroy(); }
}
