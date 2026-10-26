package dev.ot2.port;

import android.app.GameManager;
import android.content.Context;
import android.os.PowerManager;
import android.os.SystemClock;
import java.io.File;

final class FrameRatePolicy {
    private final PowerManager power;
    private final GameManager game;
    private final File force30;
    private final File experimental60;
    private long sampledAt=-2000;
    private int target=30;
    FrameRatePolicy(Context context){power=context.getSystemService(PowerManager.class);game=context.getSystemService(GameManager.class);force30=new File(context.getFilesDir(),"shaders/force-30-fps");experimental60=new File(context.getFilesDir(),"shaders/experimental-60-fps");}
    int target(){
        long now=SystemClock.elapsedRealtime();if(now-sampledAt<2000)return target;sampledAt=now;
        boolean saving=power!=null&&power.isPowerSaveMode();
        try{saving|=game!=null&&game.getGameMode()==GameManager.GAME_MODE_BATTERY;}
        catch(RuntimeException unavailable){/* OEM without game service. */}
        // This title advances guest logic as fps/60: presenting below 60 is a
        // slow-motion game, not a power saving. Claim 60 so the compositor keeps
        // the display at 60 Hz instead of arbitrating down to 30. force-30-fps
        // stays as an explicit opt-in for measurement only.
        target=saving||force30.exists()?30:60;return target;
    }
}
