package dev.ot2.port;

import android.content.Context;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;

/**
 * 首次运行把随包携带的管线预热列表铺成 {@code <files>/games/<title>/shaders/vk/vk-pipeline-list.seed}。
 *
 * 引擎的预热器（native/graphics/pipeline_warmup.inc、xbg_game.cpp 的 warm list seed）只在
 * {@code vk-pipeline-list.bin} 还不存在时读这个种子，并把它当作本机列表，于是那些
 * 「别的设备已经见过」的管线会在第一场对战之前就建好，而不是在战斗中边玩边编译（卡顿）。
 *
 * ⚠️ 路径必须是 {@code shaders/vk/}：引擎的 {@code cfg.shader_directory} 指的就是这个子目录，
 * 列表写在它的父目录（{@code shaders/vk-pipeline-list.bin}），而种子读的是
 * {@code shader_directory + "/vk-pipeline-list.seed"}。第一次放在 {@code shaders/} 下，
 * 引擎直接跳过、日志是 {@code warm list=0}——种子铺了但没被采用。
 *
 * 种子只包含着色器内容键与无指针的管线状态，不含任何 GPU 厂商的二进制数据，换机型也能用：
 * 本机建不出来的记录在预热阶段失败（warm_failed），不会影响前台的正式创建。
 * 引擎侧的 {@code no-pipeline-seed} 标记可整条关掉这条路径。
 *
 * 幂等：目标文件已存在且大小一致就跳过；文件缺失（卸载重装会连目录一起删）才重铺。
 * 种子不覆盖已有列表 —— 列表是设备自己积累的，比种子更准。
 */
final class PipelineSeed {
    private static final String ASSET = "pipeline-seed/vk-pipeline-list.seed";
    private static final String NAME = "vk-pipeline-list.seed";

    static void install(Context context, File dataRoot, BundleInstaller.Progress progress) throws Exception {
        File target = new File(new File(new File(dataRoot, "shaders"), "vk"), NAME);
        long assetSize;
        try (InputStream probe = context.getAssets().open(ASSET)) {
            assetSize = copyOf(probe);
        } catch (IOException missing) {
            // 未随包发布（开发期热推流程）时什么都不做。
            return;
        }
        if (target.isFile() && target.length() == assetSize) return;
        File parent = target.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("着色器目录不可用");
        if (progress != null) progress.update("正在安装管线缓存…");
        File tmp = new File(target.getPath() + ".tmp");
        long copied = 0;
        try (InputStream raw = context.getAssets().open(ASSET);
             FileOutputStream out = new FileOutputStream(tmp)) {
            byte[] buffer = new byte[64 * 1024];
            int read;
            while ((read = raw.read(buffer)) > 0) { out.write(buffer, 0, read); copied += read; }
            out.getFD().sync();
        }
        if (copied != assetSize || !tmp.renameTo(target)) {
            tmp.delete();
            throw new IOException("管线缓存写入失败");
        }
    }

    private static long copyOf(InputStream input) throws IOException {
        byte[] buffer = new byte[64 * 1024];
        long total = 0;
        int read;
        while ((read = input.read(buffer)) > 0) total += read;
        return total;
    }
}
