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
 * A floating switch for the performance HUD.
 *
 * The HUD is off by default here. It is pinned to the top-left corner, which is
 * exactly where this title draws its own countdown and question header, so a HUD
 * that is always on hides the one number the player is actually playing against.
 * The diagnostics still have to be one gesture away, which is what this handle is
 * for: same gesture model as the on-screen pad's switch, so there is only one
 * thing to learn -- tap toggles, drag moves, and a thumb that lands here never
 * becomes a guest touch underneath it.
 */
final class HudToggleView extends View {
    interface OnToggle { void onToggled(boolean enabled); }

    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF body=new RectF();
    private final OnToggle callback;
    private final float slop;
    private final int size;
    private boolean hudOn;
    private boolean dragging;
    private float downRawX,downRawY;
    private int startLeft,startTop;

    HudToggleView(Context context,OnToggle callback){
        super(context);
        this.callback=callback;
        slop=ViewConfiguration.get(context).getScaledTouchSlop();
        size=dp(46);
        setContentDescription("性能面板开关");
        setClickable(true);
        setFocusable(false);
    }

    /** Reflects the HUD state; kept separate from the callback so a host can force it off. */
    void setHudOn(boolean on){
        if(hudOn==on)return;
        hudOn=on;invalidate();
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
        hudOn=!hudOn;
        invalidate();
        performHapticFeedback(hudOn?HapticFeedbackConstants.CONTEXT_CLICK:HapticFeedbackConstants.KEYBOARD_TAP);
        if(callback!=null)callback.onToggled(hudOn);
    }

    @Override protected void onDraw(Canvas canvas){
        super.onDraw(canvas);
        final float cx=getWidth()/2f,cy=getHeight()/2f;
        final float radius=Math.min(getWidth(),getHeight())/2f-dp(2);
        // Same disc as the pad switch: this sits over gameplay, and a bare outline
        // disappears against a bright sky or a white menu.
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(hudOn?0xD01B3B38:0xB0101519);
        canvas.drawCircle(cx,cy,radius,paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(dp(1.4f));
        paint.setColor(hudOn?0xFF6CE3CE:0x99FFFFFF);
        canvas.drawCircle(cx,cy,radius,paint);

        // A small screen with three bars inside: the HUD is a readout, and a readout
        // is easier to tell apart from the gamepad disc next to it than another glyph
        // of the same family. Drawn rather than shipped so it stays legible at 46 dp
        // without a density-specific asset.
        final float sw=radius*1.30f,sh=radius*0.94f;
        body.set(cx-sw/2f,cy-sh/2f,cx+sw/2f,cy+sh/2f);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(dp(1.3f));
        paint.setColor(hudOn?0xFFB9FFF2:0xCCE8F0F4);
        final float corner=sh*0.26f;
        canvas.drawRoundRect(body,corner,corner,paint);

        paint.setStyle(Paint.Style.FILL);
        final float base=cy+sh*0.36f;
        final float w=dp(1.9f);
        final float[] heights={sh*0.30f,sh*0.52f,sh*0.40f};
        for(int i=0;i<3;i++){
            final float x=cx+(i-1)*sw*0.24f;
            body.set(x-w/2f,base-heights[i],x+w/2f,base);
            canvas.drawRoundRect(body,w*0.5f,w*0.5f,paint);
        }
    }
}
