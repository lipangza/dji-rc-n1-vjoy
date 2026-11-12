package com.NaN.dji_vjoy;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.util.AttributeSet;
import android.view.MotionEvent;
import android.view.View;

/**
 * 触控虚拟摇杆：左半屏控制左杆，右半屏控制右杆，支持双指同时操作。
 * 摇杆值范围 [-660, 660]，与遥控器一致。
 *
 * 布局可调（由设置页传入）：整体大小、左右两杆各自的左右位置、共同上下位置。
 * 跟手策略：手指按在圆盘外时，该杆圆心当次吸附到手指位置，松手后回到配置位置，
 * 这样按在哪里都能立即响应，不会先跳到圆的边缘。
 */
public class TouchStickView extends View {

    public interface OnStickListener {
        void onLeftStick(int h, int v);
        void onRightStick(int h, int v);
    }

    private static final int MAX = 660;

    /** 大小滑块默认 60（对应原来的尺寸），滑块范围 20~150 */
    private static final float SIZE_BASE = 60f;

    private OnStickListener listener;

    // 配置（来自设置页）
    private int sizePct = 60;     // 圆盘大小
    private int padLX = 50;       // 左杆在左半屏内的横向位置 0~100
    private int padRX = 50;       // 右杆在右半屏内的横向位置 0~100
    private int padY = 50;        // 两杆共同纵向位置 0~100

    // 配置中心（静止位置）
    private float lcx, lcy;
    private float rcx, rcy;

    // 当前使用的圆心（被按住且手指在圆外时会吸附到手指位置）
    private float lUseX, lUseY, rUseX, rUseY;

    // 旋钮位置
    private float lx, ly;
    private float rx, ry;

    private int leftId = -1;
    private int rightId = -1;
    private int leftH = 0, leftV = 0;
    private int rightH = 0, rightV = 0;

    private float baseRadius = 0;
    private float knobRadius = 0;

    private Paint ringPaint, knobPaint, basePaint, textPaint;

    public TouchStickView(Context context) {
        super(context);
        init();
    }

    public TouchStickView(Context context, AttributeSet attrs) {
        super(context, attrs);
        init();
    }

    public TouchStickView(Context context, AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
        init();
    }

    private void init() {
        ringPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        ringPaint.setStyle(Paint.Style.STROKE);
        ringPaint.setStrokeWidth(6f);
        ringPaint.setColor(Color.argb(150, 0, 122, 255));

        basePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        basePaint.setStyle(Paint.Style.FILL);
        basePaint.setColor(Color.argb(30, 0, 122, 255));

        knobPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        knobPaint.setStyle(Paint.Style.FILL);
        knobPaint.setColor(Color.argb(210, 0, 122, 255));

        textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        textPaint.setColor(Color.argb(120, 60, 60, 67));
        textPaint.setTextSize(34f);
        textPaint.setTextAlign(Paint.Align.CENTER);
    }

    public void setOnStickListener(OnStickListener l) {
        this.listener = l;
    }

    /**
     * 设置摇杆布局。参数都是百分比：
     *
     * @param size      圆盘大小 20~150（60 等于原来的尺寸）
     * @param leftX     左杆在左半屏内的横向位置 0~100（50 = 居中）
     * @param rightX    右杆在右半屏内的横向位置 0~100（50 = 居中）
     * @param y          两杆共同的纵向位置 0~100（50 = 垂直居中）
     */
    public void setPadLayout(int size, int leftX, int rightX, int y) {
        this.sizePct = clampInt(size, 20, 150);
        this.padLX = clampInt(leftX, 0, 100);
        this.padRX = clampInt(rightX, 0, 100);
        this.padY = clampInt(y, 0, 100);
        recomputeLayout();
        invalidate();
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        recomputeLayout();
    }

    private void recomputeLayout() {
        int w = getWidth();
        int h = getHeight();
        if (w <= 0 || h <= 0) return;

        float half = w / 2f;

        // 基础尺寸再按滑块缩放；上限保证圆盘不会撑破可用区域
        float base = Math.min(half * 0.42f, h * 0.32f);
        baseRadius = base * (sizePct / SIZE_BASE);

        float maxR = Math.min(half * 0.48f, h * 0.45f);
        if (baseRadius > maxR) baseRadius = maxR;
        float minR = Math.min(half * 0.15f, h * 0.12f);
        if (baseRadius < minR) baseRadius = minR;

        knobRadius = baseRadius * 0.34f;

        // 横向位置：各自在自己那一半宽度内滑动
        lcx = half * (padLX / 100f);
        rcx = half + half * (padRX / 100f);

        // 夹在画面内，圆盘不要出界
        lcx = Math.max(baseRadius, Math.min(half - baseRadius, lcx));
        rcx = Math.max(half + baseRadius, Math.min(w - baseRadius, rcx));

        float cy = h * (padY / 100f);
        cy = Math.max(baseRadius, Math.min(h - baseRadius, cy));

        lcy = cy;
        rcy = cy;

        // 松手状态：使用配置中心
        if (leftId < 0) {
            lUseX = lcx;
            lUseY = lcy;
            lx = lcx;
            ly = lcy;
        }
        if (rightId < 0) {
            rUseX = rcx;
            rUseY = rcy;
            rx = rcx;
            ry = rcy;
        }
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        drawStick(canvas, lUseX, lUseY, lx, ly, "左杆");
        drawStick(canvas, rUseX, rUseY, rx, ry, "右杆");
    }

    private void drawStick(Canvas canvas, float cx, float cy, float kx, float ky, String label) {
        canvas.drawCircle(cx, cy, baseRadius, basePaint);
        canvas.drawCircle(cx, cy, baseRadius, ringPaint);
        canvas.drawCircle(kx, ky, knobRadius, knobPaint);
        canvas.drawText(label, cx, cy + baseRadius + 46f, textPaint);
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int action = e.getActionMasked();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int idx = e.getActionIndex();
                float x = e.getX(idx);
                float y = e.getY(idx);
                boolean isLeft = x < getWidth() / 2f;

                // 圆心吸附：手指落在圆盘外时，把该杆的圆心临时放到手指处，保证跟手
                float cx = isLeft ? lcx : rcx;
                float cy = isLeft ? lcy : rcy;
                float dist = (float) Math.hypot(x - cx, y - cy);
                if (dist > baseRadius) {
                    cx = x;
                    cy = y;
                }

                if (isLeft) {
                    leftId = e.getPointerId(idx);
                    lUseX = cx;
                    lUseY = cy;
                    updateLeft(x, y);
                } else {
                    rightId = e.getPointerId(idx);
                    rUseX = cx;
                    rUseY = cy;
                    updateRight(x, y);
                }
                break;
            }
            case MotionEvent.ACTION_MOVE: {
                for (int i = 0; i < e.getPointerCount(); i++) {
                    int id = e.getPointerId(i);
                    float x = e.getX(i);
                    float y = e.getY(i);
                    if (id == leftId) {
                        updateLeft(x, y);
                    } else if (id == rightId) {
                        updateRight(x, y);
                    }
                }
                break;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                int idx = e.getActionIndex();
                int id = e.getPointerId(idx);
                if (id == leftId) {
                    leftId = -1;
                    resetLeft();
                } else if (id == rightId) {
                    rightId = -1;
                    resetRight();
                }
                break;
            }
            case MotionEvent.ACTION_CANCEL: {
                leftId = -1;
                rightId = -1;
                resetLeft();
                resetRight();
                break;
            }
            default:
                break;
        }
        invalidate();
        return true;
    }

    private void updateLeft(float x, float y) {
        float dx = x - lUseX;
        float dy = y - lUseY;
        float dist = (float) Math.hypot(dx, dy);
        if (dist > baseRadius) {
            dx = dx / dist * baseRadius;
            dy = dy / dist * baseRadius;
        }
        lx = lUseX + dx;
        ly = lUseY + dy;
        leftH = clamp(Math.round(dx / baseRadius * MAX));
        leftV = clamp(Math.round(-dy / baseRadius * MAX));
        if (listener != null) listener.onLeftStick(leftH, leftV);
    }

    private void updateRight(float x, float y) {
        float dx = x - rUseX;
        float dy = y - rUseY;
        float dist = (float) Math.hypot(dx, dy);
        if (dist > baseRadius) {
            dx = dx / dist * baseRadius;
            dy = dy / dist * baseRadius;
        }
        rx = rUseX + dx;
        ry = rUseY + dy;
        rightH = clamp(Math.round(dx / baseRadius * MAX));
        rightV = clamp(Math.round(-dy / baseRadius * MAX));
        if (listener != null) listener.onRightStick(rightH, rightV);
    }

    private void resetLeft() {
        lUseX = lcx;
        lUseY = lcy;
        lx = lcx;
        ly = lcy;
        leftH = 0;
        leftV = 0;
        if (listener != null) listener.onLeftStick(0, 0);
    }

    private void resetRight() {
        rUseX = rcx;
        rUseY = rcy;
        rx = rcx;
        ry = rcy;
        rightH = 0;
        rightV = 0;
        if (listener != null) listener.onRightStick(0, 0);
    }

    private int clamp(int v) {
        if (v > MAX) return MAX;
        if (v < -MAX) return -MAX;
        return v;
    }

    private static int clampInt(int v, int lo, int hi) {
        if (v < lo) return lo;
        if (v > hi) return hi;
        return v;
    }
}
