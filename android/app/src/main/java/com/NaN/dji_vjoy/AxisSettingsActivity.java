package com.NaN.dji_vjoy;

import android.content.SharedPreferences;
import android.os.Bundle;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;

import androidx.appcompat.app.AppCompatActivity;
import androidx.appcompat.widget.AppCompatButton;

/**
 * 轴设置页：4 个轴（左杆水平 / 左杆垂直 / 右杆水平 / 右杆垂直）各自独立设置
 *   - 反向：颠倒该轴方向
 *   - 中位：50% = 不改变；越低越靠近杆上端，越高越靠近杆下端
 *   - 灵敏度：该轴自己的滑动速率（100% 最快，10% 最慢）
 *
 * 参数存 SharedPreferences("config")，键名 ax_inv{i} / ax_ctr{i} / ax_sens{i}，
 * MainActivity 读取后逐轴应用。
 */
public class AxisSettingsActivity extends AppCompatActivity {

    private static final int AXIS_COUNT = 4;

    private final Switch[] swInv = new Switch[AXIS_COUNT];
    private final SeekBar[] sbCtr = new SeekBar[AXIS_COUNT];
    private final SeekBar[] sbSens = new SeekBar[AXIS_COUNT];
    private final TextView[] tvCtr = new TextView[AXIS_COUNT];
    private final TextView[] tvSens = new TextView[AXIS_COUNT];

    private SharedPreferences sp = null;

    // 触控摇杆布局控件
    private SeekBar sbPadSize, sbPadLx, sbPadRx, sbPadY;
    private TextView tvPadSize, tvPadLx, tvPadRx, tvPadY;

    // 控件 id 集中在这里，避免四段重复代码
    private static final int[] SW_INV_IDS = {
            R.id.sw_inv0, R.id.sw_inv1, R.id.sw_inv2, R.id.sw_inv3
    };
    private static final int[] SB_CTR_IDS = {
            R.id.sb_ctr0, R.id.sb_ctr1, R.id.sb_ctr2, R.id.sb_ctr3
    };
    private static final int[] SB_SENS_IDS = {
            R.id.sb_sens0, R.id.sb_sens1, R.id.sb_sens2, R.id.sb_sens3
    };
    private static final int[] TV_CTR_IDS = {
            R.id.tv_ctr0, R.id.tv_ctr1, R.id.tv_ctr2, R.id.tv_ctr3
    };
    private static final int[] TV_SENS_IDS = {
            R.id.tv_sens0, R.id.tv_sens1, R.id.tv_sens2, R.id.tv_sens3
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_axis_settings);

        sp = getSharedPreferences("config", MODE_PRIVATE);

        int defaultSens = sp.getInt("sens", 100);
        int defaultHover = sp.getInt("hover", 50);

        for (int i = 0; i < AXIS_COUNT; i++) {
            swInv[i] = findViewById(SW_INV_IDS[i]);
            sbCtr[i] = findViewById(SB_CTR_IDS[i]);
            sbSens[i] = findViewById(SB_SENS_IDS[i]);
            tvCtr[i] = findViewById(TV_CTR_IDS[i]);
            tvSens[i] = findViewById(TV_SENS_IDS[i]);
        }

        // 先填值，再挂监听，避免初始化时触发保存
        for (int i = 0; i < AXIS_COUNT; i++) {
            // 轴 1（左杆垂直/油门）默认沿用主界面的「油门中位」，其余轴默认 50 = 不改
            int defCtr = (i == 1) ? defaultHover : 50;
            int inv = sp.getInt("ax_inv" + i, 0);
            int ctr = sp.getInt("ax_ctr" + i, defCtr);
            int sens = sp.getInt("ax_sens" + i, defaultSens);

            swInv[i].setChecked(inv == 1);
            sbCtr[i].setProgress(ctr);
            sbSens[i].setProgress(sens);
            tvCtr[i].setText(describeCenter(ctr));
            tvSens[i].setText(sens + "%");
        }

        for (int i = 0; i < AXIS_COUNT; i++) {
            final int idx = i;

            swInv[i].setOnCheckedChangeListener((buttonView, isChecked) ->
                    sp.edit().putInt("ax_inv" + idx, isChecked ? 1 : 0).apply());

            sbCtr[i].setOnSeekBarChangeListener(new SimpleSeekListener() {
                @Override
                public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                    tvCtr[idx].setText(describeCenter(progress));
                    sp.edit().putInt("ax_ctr" + idx, progress).apply();
                }
            });

            sbSens[i].setOnSeekBarChangeListener(new SimpleSeekListener() {
                @Override
                public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                    tvSens[idx].setText(progress + "%");
                    sp.edit().putInt("ax_sens" + idx, progress).apply();
                }
            });
        }

        bindPadLayout();

        AppCompatButton reset = findViewById(R.id.btn_reset);
        reset.setOnClickListener(v -> resetAll(defaultSens, defaultHover));
    }

    /** 触控摇杆布局：大小 / 左杆左右 / 右杆左右 / 上下位置 */
    private void bindPadLayout() {
        sbPadSize = findViewById(R.id.sb_pad_size);
        sbPadLx = findViewById(R.id.sb_pad_lx);
        sbPadRx = findViewById(R.id.sb_pad_rx);
        sbPadY = findViewById(R.id.sb_pad_y);
        tvPadSize = findViewById(R.id.tv_pad_size);
        tvPadLx = findViewById(R.id.tv_pad_lx);
        tvPadRx = findViewById(R.id.tv_pad_rx);
        tvPadY = findViewById(R.id.tv_pad_y);

        int size = sp.getInt("pad_size", 60);
        int lx = sp.getInt("pad_lx", 50);
        int rx = sp.getInt("pad_rx", 50);
        int y = sp.getInt("pad_y", 50);

        sbPadSize.setProgress(size);
        sbPadLx.setProgress(lx);
        sbPadRx.setProgress(rx);
        sbPadY.setProgress(y);
        tvPadSize.setText(String.valueOf(size));
        tvPadLx.setText(String.valueOf(lx));
        tvPadRx.setText(String.valueOf(rx));
        tvPadY.setText(String.valueOf(y));

        bindPadSeek(sbPadSize, tvPadSize, "pad_size", 20);
        bindPadSeek(sbPadLx, tvPadLx, "pad_lx", 0);
        bindPadSeek(sbPadRx, tvPadRx, "pad_rx", 0);
        bindPadSeek(sbPadY, tvPadY, "pad_y", 0);
    }

    private void bindPadSeek(SeekBar sb, TextView tv, String key, int min) {
        sb.setOnSeekBarChangeListener(new SimpleSeekListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                int v = Math.max(min, progress);
                sp.edit().putInt(key, v).apply();
                tv.setText(String.valueOf(v));
            }
        });
    }

    /** 中位值的文字说明：50 是中性，往两端说明偏向哪边 */
    private String describeCenter(int pct) {
        if (pct == 50) return "50%";
        if (pct < 50) return pct + "% ↑";
        return pct + "% ↓";
    }

    private void resetAll(int defaultSens, int defaultHover) {
        for (int i = 0; i < AXIS_COUNT; i++) {
            int defCtr = (i == 1) ? defaultHover : 50;
            sp.edit()
                    .putInt("ax_inv" + i, 0)
                    .putInt("ax_ctr" + i, defCtr)
                    .putInt("ax_sens" + i, defaultSens)
                    .apply();

            swInv[i].setChecked(false);
            sbCtr[i].setProgress(defCtr);
            sbSens[i].setProgress(defaultSens);
            tvCtr[i].setText(describeCenter(defCtr));
            tvSens[i].setText(defaultSens + "%");
        }

        // 触控摇杆布局也一起复位
        sp.edit()
                .putInt("pad_size", 60)
                .putInt("pad_lx", 50)
                .putInt("pad_rx", 50)
                .putInt("pad_y", 50)
                .apply();
        if (sbPadSize != null) {
            sbPadSize.setProgress(60);
            sbPadLx.setProgress(50);
            sbPadRx.setProgress(50);
            sbPadY.setProgress(50);
            tvPadSize.setText("60");
            tvPadLx.setText("50");
            tvPadRx.setText("50");
            tvPadY.setText("50");
        }
    }

    /** 只关心 onProgressChanged 时的空实现，省掉重复的两个空方法 */
    private static abstract class SimpleSeekListener implements SeekBar.OnSeekBarChangeListener {
        @Override
        public void onStartTrackingTouch(SeekBar seekBar) {
        }

        @Override
        public void onStopTrackingTouch(SeekBar seekBar) {
        }
    }
}
