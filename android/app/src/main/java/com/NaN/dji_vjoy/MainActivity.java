package com.NaN.dji_vjoy;

import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;

import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.View;
import android.view.WindowManager;
import android.widget.EditText;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.widget.AppCompatButton;

import java.io.IOException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.SocketException;
import java.net.UnknownHostException;

import dji.common.Stick;
import dji.common.error.DJIError;
import dji.common.error.DJISDKError;
import dji.common.remotecontroller.HardwareState;
import dji.sdk.base.BaseComponent;
import dji.sdk.base.BaseProduct;
import dji.sdk.products.Aircraft;
import dji.sdk.remotecontroller.RemoteController;
import dji.sdk.sdkmanager.DJISDKInitEvent;
import dji.sdk.sdkmanager.DJISDKManager;
import utils.Utils;

public class MainActivity extends AppCompatActivity {
    private TextView tv_registration = null;
    private TextView tv_rc_connection = null;
    private EditText et_serverIP = null;
    private AppCompatButton btn_start = null;
    private AppCompatButton btn_touch = null;
    private TouchStickView touchStickView = null;
    private AppCompatButton btn_settings = null;

    private SharedPreferences sp = null;
    private boolean touchMode = false;

    // ---- 每轴独立参数（设置页可调）----
    // 顺序与 outStick/target 一致：0=左水平 1=左垂直 2=右水平 3=右垂直
    private final int[] axInv = new int[]{0, 0, 0, 0};       // 1 = 反向
    private final int[] axCtr = new int[]{50, 50, 50, 50};   // 物理中位对应的行程百分比，50 = 不改
    private final int[] axSens = new int[]{100, 100, 100, 100}; // 每轴灵敏度（滑动速率）

    // 触控摇杆当前值
    private int leftH = 0, leftV = 0, rightH = 0, rightV = 0;

    // 触控模式的固定节拍发送（60Hz）。触摸事件频率本来就抖（60~120Hz 不等），
    // 靠事件驱动会让包间隔忽长忽短；改成定时器按 16ms 稳定出包，节奏最匀。
    private static final int STICK_SEND_INTERVAL_MS = 16;
    private volatile boolean stickLoopRunning = false;

    // ---- 灵敏度 = 摇杆的滑动速率（不是幅值缩放）----
    // 原始量程 ±660；输出值按“每秒最多变化多少”向目标值逼近，
    // 灵敏度越低滑得越慢，但只要一直打杆，最终一定能打到底。
    private static final double STICK_FULL_RANGE = 660.0;
    private static final double STICK_T_FULL_MAX = 1.85;  // 灵敏度最低时，从中心打到满杆约 1.85 秒
    private static final double STICK_T_FULL_MIN = 0.18;  // 灵敏度 100% 时约 0.18 秒（不再一帧瞬移）
    private long lastStickTime = 0;
    private final float[] outStick = new float[]{0f, 0f, 0f, 0f};  // 已输出值：左水平/左垂直/右水平/右垂直

    private Handler mHandler = null;
    private Aircraft mProduct = null;
    private DatagramSocket socket = null;
    private InetAddress serverAddress = null;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().setFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON, WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        setContentView(R.layout.activity_main);

        mHandler = new Handler(Looper.getMainLooper());
        tv_registration = findViewById(R.id.tv_registration);
        tv_rc_connection = findViewById(R.id.tv_rc_connection);
        et_serverIP = findViewById(R.id.et_serverIP);
        btn_start = findViewById(R.id.btn_start);
        btn_touch = findViewById(R.id.btn_touch);
        touchStickView = findViewById(R.id.touchStickView);
        btn_settings = findViewById(R.id.btn_settings);

        // 设置页：每个轴单独调中位 / 反向 / 灵敏度
        btn_settings.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                startActivity(new Intent(MainActivity.this, AxisSettingsActivity.class));
            }
        });

        // 恢复本地保存的 IP
        sp = getSharedPreferences("config", MODE_PRIVATE);
        et_serverIP.setText(sp.getString("server_ip", "192.168.1.214"));

        // 触控摇杆回调：只更新目标值，真正的出包由 60Hz 定时器负责
        touchStickView.setOnStickListener(new TouchStickView.OnStickListener() {
            @Override
            public void onLeftStick(int h, int v) {
                leftH = h;
                leftV = v;
            }

            @Override
            public void onRightStick(int h, int v) {
                rightH = h;
                rightV = v;
            }
        });

        // 触控模式开关
        btn_touch.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                touchMode = !touchMode;
                if (touchMode) {
                    ensureSocket();
                    touchStickView.setVisibility(View.VISIBLE);
                    btn_touch.setText("触控模式：开");
                    btn_touch.setBackgroundResource(R.drawable.bg_status_ok);
                    btn_touch.setTextColor(getColor(R.color.ok));
                    startStickLoop();
                    showToast("已开启触控模式：左半屏=左杆，右半屏=右杆");
                } else {
                    stopStickLoop();
                    touchStickView.setVisibility(View.GONE);
                    btn_touch.setText("触控模式：关");
                    btn_touch.setBackgroundResource(R.drawable.bg_btn_touch);
                    btn_touch.setTextColor(getColor(R.color.accent));
                }
            }
        });

        // 灵敏度与油门中位都移到设置页逐轴设置，主界面不再放全局滑块

        // 权限
        Utils.checkAndRequestPermissions(this);
        // 注册 SDK
        startSDKRegistration();

        // 开始模拟
        btn_start.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                String serverIPStr = String.valueOf(et_serverIP.getText());
                sp.edit().putString("server_ip", serverIPStr).apply();

                if (Utils.isValidIP(serverIPStr)) {
                    try {
                        if (socket == null) {
                            socket = new DatagramSocket(8456);
                        }
                        serverAddress = InetAddress.getByName(serverIPStr);
                    } catch (SocketException | UnknownHostException e) {
                        e.printStackTrace();
                    }

                    if (touchMode) {
                        showToast("触控模式已就绪，请用屏幕虚拟摇杆操作");
                        return;
                    }
                    if (mProduct == null) {
                        showToast("遥控器未连接，请先连接 RC-N1");
                        return;
                    }

                    RemoteController mController = mProduct.getRemoteController();
                    mController.setHardwareStateCallback(new HardwareState.HardwareStateCallback() {
                        @Override
                        public void onUpdate(@NonNull HardwareState hardwareState) {
                            Stick leftStick = hardwareState.getLeftStick();
                            Stick rightStick = hardwareState.getRightStick();
                            // 灵敏度只影响“滑动速率”，不打折幅值：满杆照样是满量程
                            // 左杆垂直先过油门中位映射，让物理中位对应悬停油门
                            int[] sm = smoothStick(new int[]{
                                    applyAxis(0, leftStick.getHorizontalPosition()),
                                    applyAxis(1, leftStick.getVerticalPosition()),
                                    applyAxis(2, rightStick.getHorizontalPosition()),
                                    applyAxis(3, rightStick.getVerticalPosition())
                            });
                            int[] left = new int[]{sm[0], sm[1]};
                            int[] right = new int[]{sm[2], sm[3]};
                            byte[] leftByte = Utils.ByteArrayConcat(Utils.Int16ToByte(left[0]), Utils.Int16ToByte(left[1]));
                            byte[] rightByte = Utils.ByteArrayConcat(Utils.Int16ToByte(right[0]), Utils.Int16ToByte(right[1]));
                            byte[] stickData = Utils.ByteArrayConcat(leftByte, rightByte);

                            byte[] leftDialData = Utils.Int16ToByte(hardwareState.getLeftDial());
                            HardwareState.RightDial rd = hardwareState.getRightDial();
                            byte[] rightDialData = Utils.Int16ToByte(rd != null ? rd.getValue() : 0);

                            // 按键组1：bit0 C1 / bit1 C2 / bit2 C3 / bit3 Fn / bit4 返航
                            //          bit5 五维按下 / bit6 回放 / bit7 暂停
                            int b1 = 0;
                            if (isClick(hardwareState.getC1Button()))       b1 |= 0x01;
                            if (isClick(hardwareState.getC2Button()))       b1 |= 0x02;
                            if (isClick(hardwareState.getC3Button()))       b1 |= 0x04;
                            if (isClick(hardwareState.getFunctionButton())) b1 |= 0x08;
                            if (isClick(hardwareState.getGoHomeButton()))   b1 |= 0x10;
                            if (isClick(hardwareState.getPlaybackButton())) b1 |= 0x40;
                            if (isClick(hardwareState.getPauseButton()))    b1 |= 0x80;

                            // 按键组2：bit0 录像 / bit1 快门 / bit2 拍照录像切换 / bit3 菜单
                            //          bit4 五维上 / bit5 五维下 / bit6 五维左 / bit7 五维右
                            int b2 = 0;
                            if (isClick(hardwareState.getRecordButton()))              b2 |= 0x01;
                            if (isClick(hardwareState.getShutterButton()))             b2 |= 0x02;
                            if (isClick(hardwareState.getPhotoAndVideoToggleButton())) b2 |= 0x04;
                            if (isClick(hardwareState.getMenuButton()))                b2 |= 0x08;

                            HardwareState.FiveDButton fd = hardwareState.getFiveDButton();
                            if (fd != null) {
                                if (fd.isClicked()) b1 |= 0x20;
                                HardwareState.FiveDButtonDirection v = fd.getVerticalDirection();
                                HardwareState.FiveDButtonDirection h = fd.getHorizontalDirection();
                                if (v == HardwareState.FiveDButtonDirection.POSITIVE) b2 |= 0x10;  // 上
                                if (v == HardwareState.FiveDButtonDirection.NEGATIVE) b2 |= 0x20;  // 下
                                if (h == HardwareState.FiveDButtonDirection.NEGATIVE) b2 |= 0x40;  // 左
                                if (h == HardwareState.FiveDButtonDirection.POSITIVE) b2 |= 0x80;  // 右
                            }

                            // 飞行模式开关位置：0/1/2，0xFF 表示本机型没有
                            int flightMode = 0xFF;
                            HardwareState.FlightModeSwitch fs = hardwareState.getFlightModeSwitch();
                            if (fs == HardwareState.FlightModeSwitch.POSITION_ONE)        flightMode = 0;
                            else if (fs == HardwareState.FlightModeSwitch.POSITION_TWO)   flightMode = 1;
                            else if (fs == HardwareState.FlightModeSwitch.POSITION_THREE) flightMode = 2;

                            byte[] sendData = Utils.ByteArrayConcat(
                                    Utils.ByteArrayConcat(Utils.ByteArrayConcat(stickData, leftDialData), rightDialData),
                                    new byte[]{(byte) b1, (byte) b2, (byte) flightMode, (byte) 0xFF});
                            if (socket == null || serverAddress == null) return;
                            DatagramPacket packet = new DatagramPacket(sendData, 0, sendData.length, serverAddress, 8456);
                            try {
                                socket.send(packet);
                            } catch (IOException e) {
                                e.printStackTrace();
                            }
                        }
                    });
                }
            }
        });

    }

    /**
     * DJI 按键安全判定：subclass 可能为 null，或本机型根本没有这个键（isPresent=false），
     * 这两种情况一律当作“未按下”，避免个别机型回调里抛空指针。
     */
    private static boolean isClick(HardwareState.Button btn) {
        return btn != null && btn.isPresent() && btn.isClicked();
    }

    /**
     * 按当前输入的 IP 建立/复用 UDP 通道。触控模式下不点「开始模拟」也能直接发数据。
     */
    private void ensureSocket() {
        String serverIPStr = String.valueOf(et_serverIP.getText());
        if (!Utils.isValidIP(serverIPStr)) {
            showToast("IP 格式不正确");
            return;
        }
        try {
            if (socket == null) {
                socket = new DatagramSocket(8456);
            }
            serverAddress = InetAddress.getByName(serverIPStr);
            sp.edit().putString("server_ip", serverIPStr).apply();
        } catch (SocketException | UnknownHostException e) {
            e.printStackTrace();
            showToast("网络初始化失败：" + e.getMessage());
        }
    }

    /**
     * 逐轴后处理：先做中位映射，再按需反向。
     * 中位、灵敏度都在设置页里逐轴设定，主界面不再有全局滑块。
     */
    private int applyAxis(int idx, int v) {
        int ctr = axCtr[idx];
        if (ctr != 50) v = remapCenter(v, ctr);
        if (axInv[idx] == 1) v = -v;
        return v;
    }

    /**
     * 中位映射：把该轴的“物理中位”搬到指定的行程百分比上，两端照样打满。
     *
     * pct=50 → 原样返回（行为不变）
     * pct=35 → 杆在中位输出 35% 行程；两端仍然能到 0% / 100%（中心点平移 + 两侧按比例拉伸）
     */
    private int remapCenter(int v, int pct) {
        if (pct == 50) return v;
        // 杆值坐标系：+660 = 上推，-660 = 下拉；PC 端写轴时取反后映射到 0..32767，
        // 所以这里的中位用 1-2h 表示，才能让“输出百分比”正好等于 pct。
        double center = (1.0 - pct / 100.0 * 2.0) * STICK_FULL_RANGE;
        double ratio = v / STICK_FULL_RANGE;
        double span = (v >= 0) ? (STICK_FULL_RANGE - center) : (center + STICK_FULL_RANGE);
        double out = center + span * ratio;
        if (out >  STICK_FULL_RANGE) out =  STICK_FULL_RANGE;
        if (out < -STICK_FULL_RANGE) out = -STICK_FULL_RANGE;
        return (int) Math.round(out);
    }

    /**
     * 把目标摇杆值按“滑动速率”平滑成实际输出值。
     *
     * 灵敏度在这里不是缩放幅值，而是决定摇杆值每秒钟最多能变化多少：
     *   sens=100% → tFull≈0.18s，仍然跟手，但不再是"一帧瞬移"
     *   sens=80%  → 从中心打到满杆约 0.51 秒
     *   sens=50%  → 约 1.02 秒，但最终仍然能打到底
     *   sens=10%  → 约 1.68 秒滑到底，适合精细微调
     * 步长按真实时间差 dt 算，与回调频率无关（遥控器 20Hz、触控事件更密都一样）。
     */
    private int[] smoothStick(int[] target) {
        long now = SystemClock.elapsedRealtime();
        double dt;
        if (lastStickTime == 0) {
            dt = 0.05;                     // 首帧按 50ms 估
        } else {
            dt = (now - lastStickTime) / 1000.0;
            if (dt < 0.001) dt = 0.001;
            if (dt > 0.2) dt = 0.2;        // 卡顿或切后台后不要一下跳太远
        }
        lastStickTime = now;

        int[] out = new int[4];
        for (int i = 0; i < 4; i++) {
            // 每个轴用自己的灵敏度算步长，没单独设过的跟随设置页默认值
            double s = axSens[i] / 100.0;
            double tFull = STICK_T_FULL_MIN + (STICK_T_FULL_MAX - STICK_T_FULL_MIN) * (1.0 - s);
            if (tFull < STICK_T_FULL_MIN) tFull = STICK_T_FULL_MIN;
            double maxStep = STICK_FULL_RANGE / tFull * dt;

            float cur = outStick[i];
            float d = target[i] - cur;
            if (Math.abs(d) <= maxStep) {
                cur = target[i];           // 追上目标（含打到底、回中）
            } else {
                cur += (d > 0 ? maxStep : -maxStep);
            }
            outStick[i] = cur;
            out[i] = Math.round(cur);
        }
        return out;
    }

    /**
     * 触控模式：把左右摇杆值打包成 16 字节 UDP 包（格式与遥控器一致）发送。
     *
     * 只在主线程直接 send：UDP 发一个 16 字节的包不会阻塞（原实现每帧新建一个 Thread，
     * 高频手势下线程创建/调度本身就吃掉几十毫秒，反而拖慢出包）。
     */
    private void sendStick() {
        if (socket == null || serverAddress == null) ensureSocket();
        if (socket == null || serverAddress == null) return;

        int[] target = new int[]{
                applyAxis(0, leftH),
                applyAxis(1, leftV),
                applyAxis(2, rightH),
                applyAxis(3, rightV)
        };

        int[] smoothed = smoothStick(target);
        int lh = smoothed[0];
        int lv = smoothed[1];
        int rh = smoothed[2];
        int rv = smoothed[3];

        byte[] leftByte = Utils.ByteArrayConcat(Utils.Int16ToByte(lh), Utils.Int16ToByte(lv));
        byte[] rightByte = Utils.ByteArrayConcat(Utils.Int16ToByte(rh), Utils.Int16ToByte(rv));
        byte[] stickData = Utils.ByteArrayConcat(leftByte, rightByte);
        byte[] leftDialData = Utils.Int16ToByte(0);
        byte[] rightDialData = Utils.Int16ToByte(0);
        // 16 字节新协议：末 4 字节依次为 按键组1 / 按键组2 / 飞行模式 / 保留
        byte[] sendData = Utils.ByteArrayConcat(
                Utils.ByteArrayConcat(Utils.ByteArrayConcat(stickData, leftDialData), rightDialData),
                new byte[]{0, 0, (byte) 0xFF, (byte) 0xFF});

        try {
            socket.send(new DatagramPacket(sendData, 0, sendData.length, serverAddress, 8456));
        } catch (Exception e) {
            e.printStackTrace();
        }
    }

    /** 触控模式的固定节拍：每 16ms 出一帧，不管这期间有没有新触摸事件 */
    private final Runnable stickLoopRunnable = new Runnable() {
        @Override
        public void run() {
            if (!stickLoopRunning) return;
            sendStick();
            mHandler.postDelayed(this, STICK_SEND_INTERVAL_MS);
        }
    };

    private void startStickLoop() {
        if (stickLoopRunning) return;
        stickLoopRunning = true;
        mHandler.removeCallbacks(stickLoopRunnable);
        mHandler.post(stickLoopRunnable);
    }

    private void stopStickLoop() {
        stickLoopRunning = false;
        mHandler.removeCallbacks(stickLoopRunnable);
    }

    /**
     * 读取每轴独立参数（设置页保存的），启动时和从设置页返回时各读一次。
     * 键名与 AxisSettingsActivity 保持一致：ax_inv{i} / ax_ctr{i} / ax_sens{i}
     */
    private void loadAxisSettings() {
        int defaultSens = sp.getInt("sens", 100);
        int defaultHover = sp.getInt("hover", 50);   // 兼容老版本主界面设过的油门中位
        for (int i = 0; i < 4; i++) {
            axInv[i] = sp.getInt("ax_inv" + i, 0);
            axCtr[i] = sp.getInt("ax_ctr" + i, (i == 1) ? defaultHover : 50);
            axSens[i] = sp.getInt("ax_sens" + i, defaultSens);
        }
    }

    /** 把设置页里的触控摇杆布局（大小 / 左杆左右 / 右杆左右 / 上下）应用到 View */
    private void applyPadLayout() {
        if (touchStickView == null) return;
        touchStickView.setPadLayout(
                sp.getInt("pad_size", 60),
                sp.getInt("pad_lx", 50),
                sp.getInt("pad_rx", 50),
                sp.getInt("pad_y", 50));
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (sp != null) {
            loadAxisSettings();
            applyPadLayout();
        }
        // 回到前台且处于触控模式时，恢复 60Hz 出包
        if (touchMode) startStickLoop();
    }

    @Override
    protected void onPause() {
        stopStickLoop();   // 后台不再出包，省电也避免旧值长期占着 vJoy
        super.onPause();
    }

    private void startSDKRegistration() {
        new Thread(new Runnable() {
            @Override
            public void run() {
                showToast("registering, pls wait...");
                DJISDKManager.getInstance().registerApp(MainActivity.this.getApplicationContext(), new DJISDKManager.SDKManagerCallback() {
                    @Override
                    public void onRegister(DJIError djiError) {
                        if (djiError == DJISDKError.REGISTRATION_SUCCESS) {
                            mHandler.post(new Runnable() {
                                @Override
                                public void run() {
                                    tv_registration.setText("已注册");
                                }
                            });
                        } else {
                            showToast("SDK 注册失败：" + djiError.getDescription());
                        }
                    }

                    @Override
                    public void onProductDisconnect() {
                        showToast("设备已断开连接");
                        mProduct = null;
                        mHandler.post(new Runnable() {
                            @Override
                            public void run() {
                                tv_rc_connection.setText("设备未连接");
                                tv_rc_connection.setBackgroundResource(R.drawable.bg_status_error);
                            }
                        });
                        notifyStatusChange();
                    }

                    @Override
                    public void onProductConnect(BaseProduct baseProduct) {
                        showToast("设备已连接");
                        mProduct = (Aircraft) baseProduct;
                        mHandler.post(new Runnable() {
                            @Override
                            public void run() {
                                tv_rc_connection.setText("设备已连接");
                                tv_rc_connection.setBackgroundResource(R.drawable.bg_status_ok);
                            }
                        });
                        notifyStatusChange();
                    }

                    @Override
                    public void onProductChanged(BaseProduct baseProduct) {

                    }

                    @Override
                    public void onComponentChange(BaseProduct.ComponentKey componentKey, BaseComponent baseComponent, BaseComponent baseComponent1) {

                    }

                    @Override
                    public void onInitProcess(DJISDKInitEvent djisdkInitEvent, int i) {

                    }

                    @Override
                    public void onDatabaseDownloadProgress(long l, long l1) {

                    }
                });
            }
        }).start();
    }

    private void showToast(String toast) {
        mHandler.post(new Runnable() {
            @Override
            public void run() {
                Toast.makeText(getApplicationContext(), toast, Toast.LENGTH_SHORT).show();
            }
        });
    }

    private Runnable updateRunnable = new Runnable() {
        @Override
        public void run() {
            Intent intent = new Intent(Utils.FLAG_CONNECTION_CHANGE);
            sendBroadcast(intent);
        }
    };

    private void notifyStatusChange() {
        mHandler.removeCallbacks(updateRunnable);
        mHandler.postDelayed(updateRunnable, 500);
    }
}
