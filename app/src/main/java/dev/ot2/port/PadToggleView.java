package dev.ot2.port;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.widget.FrameLayout;

/**
 * A floating switch for the on-screen pad.
 *
 * The pad is off by default here: this title is played with a finger, and a full set
 * of rings over the picture competes with the one thing the screen is for. That leaves
 * a problem the pad never had -- how does the player ask for buttons when a screen
 * insists on a real A press? One thumb-sized handle, always on top, that toggles the
 * pad on tap and can be dragged out of the way.
 *
 * Drag and tap share one gesture, the way a chat bubble does: moving past the touch
 * slop turns the press into a drag and only a release without movement counts as a
 * tap. Either way this view consumes the event, so a thumb that lands here never
 * becomes a guest touch underneath it.
 */
final class PadToggleView extends View {
    interface OnToggle { void onToggled(boolean enabled); }

    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF body=new RectF();
    private final OnToggle callback;
    private final float slop;
    private final int size;
    private boolean padOn;
    private boolean dragging;
    private float downRawX,downRawY;
    private int startLeft,startTop;

    PadToggleView(Context context,OnToggle callback){
        super(context);
        this.callback=callback;
        slop=ViewConfiguration.get(context).getScaledTouchSlop();
        size=dp(46);
        setContentDescription("虚拟按键开关");
        setClickable(true);
        setFocusable(false);
    }

    /** Reflects the effective pad state; a connected controller can keep the pad off. */
    void setPadOn(boolean on){
        if(padOn==on)return;
        padOn=on;invalidate();
    }

    private int dp(float value){return Math.round(value*getResources().getDisplayMetrics().density);}

    @Override protected void onMeasure(int widthSpec,int heightSpec){
        setMeasuredDimension(resolveSize(size,widthSpec),resolveSize(size,heightSpec));
    }

    @Override public boolean onTouchEvent(MotionEvent e){
        switch(e.getActionMasked()){
            case MotionEvent.ACTION_DOWN: {
                FrameLayout.LayoutParams p=(FrameLayout.LayoutParams)getLayoutParams();
                if(p==null)return false;
                downRawX=e.getRawX();downRawY=e.getRawY();
                startLeft=p.leftMargin;startTop=p.topMargin;
                dragging=false;setPressed(true);
                return true;
            }
            case MotionEvent.ACTION_MOVE: {
                float dx=e.getRawX()-downRawX,dy=e.getRawY()-downRawY;
                if(!dragging&&Math.hypot(dx,dy)>slop)dragging=true;
                if(dragging)moveTo(startLeft+Math.round(dx),startTop+Math.round(dy));
                return true;
            }
            case MotionEvent.ACTION_UP: {
                setPressed(false);
                if(!dragging)toggle();
                dragging=false;
                performClick();
                return true;
            }
            case MotionEvent.ACTION_CANCEL: {
                setPressed(false);dragging=false;
                return true;
            }
            default: return super.onTouchEvent(e);
        }
    }

    @Override public boolean performClick(){super.performClick();return true;}

    private void moveTo(int left,int top){
        View parent=(View)getParent();
        FrameLayout.LayoutParams p=(FrameLayout.LayoutParams)getLayoutParams();
        if(parent==null||p==null)return;
        p.leftMargin=Math.max(0,Math.min(Math.max(0,parent.getWidth()-getWidth()),left));
        p.topMargin=Math.max(0,Math.min(Math.max(0,parent.getHeight()-getHeight()),top));
        setLayoutParams(p);
    }

    private void toggle(){
        padOn=!padOn;
        invalidate();
        performHapticFeedback(padOn?HapticFeedbackConstants.CONTEXT_CLICK:HapticFeedbackConstants.KEYBOARD_TAP);
        if(callback!=null)callback.onToggled(padOn);
    }

    @Override protected void onDraw(Canvas canvas){
        super.onDraw(canvas);
        final float cx=getWidth()/2f,cy=getHeight()/2f;
        final float radius=Math.min(getWidth(),getHeight())/2f-dp(2);
        // Dark disc under everything: the pad is drawn over gameplay, and a bare
        // outline disappears against a bright sky or a white menu.
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(padOn?0xD01B3B38:0xB0101519);
        canvas.drawCircle(cx,cy,radius,paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(dp(1.4f));
        paint.setColor(padOn?0xFF6CE3CE:0x99FFFFFF);
        canvas.drawCircle(cx,cy,radius,paint);

        // Gamepad glyph: a rounded body, a d-pad arm on the left, two face buttons
        // on the right. Drawn rather than shipped as a vector so it stays legible
        // at 46 dp without a density-specific asset and needs no extra resource.
        final float gw=radius*1.18f,gh=radius*0.74f;
        body.set(cx-gw/2f,cy-gh/2f,cx+gw/2f,cy+gh/2f);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(dp(1.3f));
        paint.setColor(padOn?0xFFB9FFF2:0xCCE8F0F4);
        final float corner=gh*0.44f;
        canvas.drawRoundRect(body,corner,corner,paint);
        final float armX=cx-gw*0.25f,arm=gh*0.15f;
        canvas.drawLine(armX-arm,cy,armX+arm,cy,paint);
        canvas.drawLine(armX,cy-arm,armX,cy+arm,paint);
        paint.setStyle(Paint.Style.FILL);
        canvas.drawCircle(cx+gw*0.19f,cy-gh*0.14f,gh*0.105f,paint);
        canvas.drawCircle(cx+gw*0.33f,cy+gh*0.07f,gh*0.105f,paint);
    }
}
