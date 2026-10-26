package dev.ot2.port;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.util.SparseArray;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;
import java.util.ArrayList;

/** One pointer owns one input route until release. Controls never leak into guest touch. */
final class GameInputView extends View {
    interface Sink {
        void pad(long buttons,int lx,int ly,int rx,int ry);
        void touch(int action,int id,float x,float y,int width,int height);
        void cancelTouch();
    }
    private static final int BUTTON=0,LEFT_STICK=1,RIGHT_STICK=2,DPAD=3;
    private static final class Control {
        final String label; final int kind; final long bit; final float x,y,r;
        Control(String label,int kind,long bit,float x,float y,float r){this.label=label;this.kind=kind;this.bit=bit;this.x=x;this.y=y;this.r=r;}
        boolean contains(float px,float py){return Math.hypot(px-x,py-y)<=r;}
    }
    private static final class Pointer {
        final Control control;final float originX,originY;float x,y;
        Pointer(Control control,float x,float y){this.control=control;this.x=x;this.y=y;
            originX=control!=null&&control.kind!=LEFT_STICK?control.x:x;
            originY=control!=null&&control.kind!=LEFT_STICK?control.y:y;
        }
    }
    private final Sink sink;
    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
    private final ArrayList<Control> controls=new ArrayList<>();
    private final SparseArray<Pointer> pointers=new SparseArray<>();
    private boolean controllerConnected;
    // The pad is an assist, not the default input. This title is played with a
    // finger, and a full set of rings sitting over the picture competes with the
    // one thing the screen is for. Off means exactly that: nothing painted, and
    // every touch falls through to the guest as touch. The floating toggle is the
    // only way in, so the state is explicit rather than guessed from idleness.
    private boolean padEnabled;
    private float scale=1,originX,originY;
    private int insetLeft,insetTop,insetRight,insetBottom;
    GameInputView(Context context,Sink sink){
        super(context);this.sink=sink;setContentDescription("游戏触屏与虚拟手柄");setClickable(true);
        setOnApplyWindowInsetsListener((view,insets)->{
            android.graphics.Insets safe=insets.getInsets(android.view.WindowInsets.Type.displayCutout()|android.view.WindowInsets.Type.systemGestures());
            if(insetLeft!=safe.left||insetTop!=safe.top||insetRight!=safe.right||insetBottom!=safe.bottom){
                insetLeft=safe.left;insetTop=safe.top;insetRight=safe.right;insetBottom=safe.bottom;rebuild();
            }return insets;
        });
    }
    static boolean gamepadSource(int sources,boolean virtual){return !virtual&&((sources&InputDevice.SOURCE_GAMEPAD)==InputDevice.SOURCE_GAMEPAD||(sources&InputDevice.SOURCE_JOYSTICK)==InputDevice.SOURCE_JOYSTICK);}
    static boolean controllerPresent(){
        for(int id:InputDevice.getDeviceIds()){
            InputDevice d=InputDevice.getDevice(id);if(d!=null&&gamepadSource(d.getSources(),d.isVirtual()))return true;
        }return false;
    }
    boolean controlsVisible(){return padEnabled&&!controllerConnected;}
    boolean setPadEnabled(boolean enabled){
        if(padEnabled==enabled)return false;
        cancel();padEnabled=enabled;invalidate();return true;
    }
    boolean setControllerConnected(boolean connected){
        if(controllerConnected==connected)return false;
        cancel();controllerConnected=connected;invalidate();return true;
    }
    void cancel(){pointers.clear();sink.pad(0,0,0,0,0);sink.cancelTouch();invalidate();}
    @Override protected void onSizeChanged(int w,int h,int oldw,int oldh){rebuild();}
    private void add(String label,int kind,long bit,float x,float y,float r){controls.add(new Control(label,kind,bit,x,y,r));}
    private void rebuild(){
        cancel();controls.clear();
        float w=Math.max(1,getWidth()-insetLeft-insetRight),h=Math.max(1,getHeight()-insetTop-insetBottom);
        // Positions are fractions of the safe content area, independent of
        // control size. Density sets comfortable bounds; compact windows cap
        // sizes to avoid overlap. D2R also scales controls in logical points
        // and lets the movement stick float around the thumb's landing point.
        scale=getResources().getDisplayMetrics().density;originX=insetLeft;originY=insetTop;w/=scale;h/=scale;
        float button=Math.min(Math.max(24,Math.min(34,h*.042f)),Math.min(w/26,h/12));
        float stick=Math.min(Math.max(48,Math.min(76,h*.105f)),Math.min(w*.085f,h*.14f));
        float gap=button*.4f,step=button*2.4f;
        float faceX=w-Math.max(w*.13f,step+button+gap),faceY=h*.63f;
        add("左摇杆",LEFT_STICK,0,Math.max(w*.14f,stick+gap),h*.64f,stick);
        add("右摇杆",RIGHT_STICK,0,w*.70f,h*.76f,stick*.72f);
        add("方向键",DPAD,0,w*.27f,h*.78f,stick*.82f);
        add("A",BUTTON,1,faceX+step,faceY,button);add("B",BUTTON,2,faceX,faceY+step,button);
        add("X",BUTTON,4,faceX,faceY-step,button);add("Y",BUTTON,8,faceX-step,faceY,button);
        float shoulderX=Math.max(w*.08f,button+gap),shoulderY=Math.min(h*.37f,faceY-step-button*2.4f);
        add("L",BUTTON,1L<<6,shoulderX,shoulderY,button);add("ZL",BUTTON,1L<<8,shoulderX+2.6f*button,shoulderY,button);
        add("R",BUTTON,1L<<7,w-shoulderX,shoulderY,button);add("ZR",BUTTON,1L<<9,w-shoulderX-2.6f*button,shoulderY,button);
        add("−",BUTTON,1L<<11,w*.45f,h*.91f,button*.85f);add("+",BUTTON,1L<<10,w*.55f,h*.91f,button*.85f);
        add("L3",BUTTON,1L<<4,w*.42f,h*.78f,button*.8f);add("R3",BUTTON,1L<<5,w*.58f,h*.78f,button*.8f);
        invalidate();
    }
    private float localX(float x){return (x-originX)/scale;}
    private float localY(float y){return (y-originY)/scale;}
    RectF controlBounds(String label){
        for(Control c:controls)if(c.label.equals(label))return new RectF(originX+(c.x-c.r)*scale,originY+(c.y-c.r)*scale,originX+(c.x+c.r)*scale,originY+(c.y+c.r)*scale);
        throw new IllegalArgumentException(label);
    }
    private Control hit(float x,float y){
        if(!padEnabled||controllerConnected)return null;
        // Explicit buttons take precedence over the expanded floating-stick zone.
        for(Control c:controls)if(c.kind!=LEFT_STICK&&c.contains(x,y))return c;
        for(Control c:controls)if(c.kind==LEFT_STICK&&Math.hypot(x-c.x,y-c.y)<=c.r*1.55f)return c;
        return null;
    }
    private boolean occupied(Control c){for(int i=0;i<pointers.size();i++)if(pointers.valueAt(i).control==c)return true;return false;}
    @Override public boolean onTouchEvent(MotionEvent e){
        int action=e.getActionMasked(),index=e.getActionIndex();
        if(action==MotionEvent.ACTION_CANCEL){cancel();return true;}
        if(action==MotionEvent.ACTION_DOWN){if(pointers.size()!=0)cancel();if(getParent()!=null)getParent().requestDisallowInterceptTouchEvent(true);}
        if(action==MotionEvent.ACTION_DOWN||action==MotionEvent.ACTION_POINTER_DOWN){
            float x=localX(e.getX(index)),y=localY(e.getY(index));int id=e.getPointerId(index);
            Control c=hit(x,y);
            // A second finger on a stick owns a silent control route, not guest touch.
            pointers.put(id,new Pointer(c,x,y));
            if(c==null)sink.touch(0,id,e.getX(index),e.getY(index),getWidth(),getHeight());
        }
        for(int i=0;i<e.getPointerCount();i++){
            int id=e.getPointerId(i);Pointer p=pointers.get(id);if(p==null)continue;
            p.x=localX(e.getX(i));p.y=localY(e.getY(i));
            boolean up=(action==MotionEvent.ACTION_UP||action==MotionEvent.ACTION_POINTER_UP)&&i==index;
            if(p.control==null&&!(i==index&&(action==MotionEvent.ACTION_DOWN||action==MotionEvent.ACTION_POINTER_DOWN)))
                sink.touch(up?2:1,id,e.getX(i),e.getY(i),getWidth(),getHeight());
            if(up)pointers.remove(id);
        }
        publishPad();if(controlsVisible())invalidate();
        if(action==MotionEvent.ACTION_UP)performClick();return true;
    }
    @Override public boolean performClick(){super.performClick();return true;}
    private static int stick(float value,float length){
        if(length<.13f)return 0;
        return Math.round(value/length*Math.min(1,(length-.13f)/.87f)*32767);
    }
    private void publishPad(){
        long buttons=0;int lx=0,ly=0,rx=0,ry=0;boolean left=false,right=false;
        for(int i=0;i<pointers.size();i++){
            Pointer p=pointers.valueAt(i);Control c=p.control;if(c==null)continue;
            float x=(p.x-p.originX)/c.r,y=(p.originY-p.y)/c.r,length=(float)Math.hypot(x,y);
            if(c.kind==BUTTON){if(c.contains(p.x,p.y))buttons|=c.bit;}
            else if(c.kind==DPAD){
                if(x<-.3f)buttons|=1L<<12;if(y>.3f)buttons|=1L<<13;
                if(x>.3f)buttons|=1L<<14;if(y<-.3f)buttons|=1L<<15;
            }else if(c.kind==LEFT_STICK&&!left){lx=stick(x,length);ly=stick(y,length);left=true;}
            else if(c.kind==RIGHT_STICK&&!right){rx=stick(x,length);ry=stick(y,length);right=true;}
        }sink.pad(buttons,lx,ly,rx,ry);
    }
    // The pad is no longer on a timer: the floating toggle is the switch, so a drawn pad
    // means the player asked for it and it stays until they ask it away. That also means
    // nothing here needs to wake the view up on its own -- a padded screen with no touch
    // is genuinely static now, which is one less invalidation per frame on a device that
    // is being asked to hold 60 fps.
    @Override protected void onDraw(Canvas canvas){
        super.onDraw(canvas);if(!controlsVisible())return;
        canvas.save();canvas.translate(originX,originY);canvas.scale(scale,scale);
        paint.setTextAlign(Paint.Align.CENTER);
        for(Control c:controls){
            float cx=c.x,cy=c.y;
            if(c.kind==LEFT_STICK)for(int i=0;i<pointers.size();i++){Pointer p=pointers.valueAt(i);if(p.control==c){
                cx=Math.max(c.r,Math.min((getWidth()-insetLeft-insetRight)/scale-c.r,p.originX));
                cy=Math.max(c.r,Math.min((getHeight()-insetTop-insetBottom)/scale-c.r,p.originY));break;
            }}
            boolean down=occupied(c);
            paint.setStyle(Paint.Style.FILL);paint.setColor(down?0x887BA9C5:0x48202B35);canvas.drawCircle(cx,cy,c.r,paint);
            paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(1.5f);paint.setColor(down?0xDDEFFFFF:0x88FFFFFF);canvas.drawCircle(cx,cy,c.r,paint);
            paint.setStyle(Paint.Style.FILL);paint.setTextSize(c.r<23?14:20);paint.setColor(0xDDEFFFFF);
            if(c.kind==DPAD){canvas.drawText("◀",c.x-31,c.y+6,paint);canvas.drawText("▶",c.x+31,c.y+6,paint);canvas.drawText("▲",c.x,c.y-22,paint);canvas.drawText("▼",c.x,c.y+36,paint);}
            else if(c.kind==LEFT_STICK||c.kind==RIGHT_STICK){
                float dx=0,dy=0;for(int i=0;i<pointers.size();i++){Pointer p=pointers.valueAt(i);if(p.control==c){dx=p.x-p.originX;dy=p.y-p.originY;break;}}
                float length=(float)Math.hypot(dx,dy),limit=c.r*.55f;if(length>limit){dx*=limit/length;dy*=limit/length;}
                paint.setColor(0x667B9AAA);canvas.drawCircle(cx+dx,cy+dy,c.r*.43f,paint);
            }else canvas.drawText(c.label,c.x,c.y-(paint.ascent()+paint.descent())/2,paint);
        }canvas.restore();
    }
}
