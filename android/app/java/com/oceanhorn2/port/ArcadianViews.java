package com.oceanhorn2.port;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ColorFilter;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.RadialGradient;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.drawable.Drawable;
import android.view.View;

/**
 * Custom vector Canvas drawables & views implementing the Oceanhorn 2 Arcadian theme:
 * deep Aegean ocean gradients, subtle nautical compass watermark, antique gold meander borders,
 * gilded glass cards, Arcadian emblem crest, and custom diamond status indicators.
 */
public final class ArcadianViews {

    public static final int COLOR_BG_DEEP      = 0xFF071626;
    public static final int COLOR_BG_MID       = 0xFF0E2F4C;
    public static final int COLOR_BG_HORIZON   = 0xFF15476B;
    public static final int COLOR_CARD_BG      = 0xD60A1B2C;
    public static final int COLOR_CARD_INNER   = 0xAA0E2438;
    public static final int COLOR_GOLD_BRIGHT  = 0xFFF4D17A;
    public static final int COLOR_GOLD_PRIMARY = 0xFFD4A446;
    public static final int COLOR_GOLD_DIM     = 0x80C8983C;
    public static final int COLOR_GOLD_SHADOW  = 0xFF7D5B1F;
    public static final int COLOR_TEXT_PRIMARY = 0xFFF7F2E7;
    public static final int COLOR_TEXT_MUTED   = 0xFFA5BCCE;
    public static final int COLOR_OK           = 0xFF39D39F;
    public static final int COLOR_WARN         = 0xFFF2B84B;
    public static final int COLOR_ERR          = 0xFFEA5A47;
    public static final int COLOR_CTA_TOP      = 0xFFE35630;
    public static final int COLOR_CTA_BOT      = 0xFFB03114;
    public static final int COLOR_SEC_TOP      = 0xFF1A4B73;
    public static final int COLOR_SEC_BOT      = 0xFF0E2C47;
    public static final int COLOR_DIS_TOP      = 0xFF1E3245;
    public static final int COLOR_DIS_BOT      = 0xFF132230;

    private ArcadianViews() {}

    public static int dp(Context ctx, float dp) {
        return Math.round(dp * ctx.getResources().getDisplayMetrics().density);
    }

    /** Fullscreen Aegean ocean gradient backdrop with subtle nautical compass watermark & gold meander strips. */
    public static final class BackdropDrawable extends Drawable {
        private final float density;
        private final Paint bgPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint glowPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint linePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint goldPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

        public BackdropDrawable(Context ctx) {
            this.density = ctx.getResources().getDisplayMetrics().density;
            linePaint.setStyle(Paint.Style.STROKE);
            goldPaint.setStyle(Paint.Style.STROKE);
        }

        @Override
        public void draw(Canvas c) {
            Rect b = getBounds();
            float w = b.width();
            float h = b.height();
            if (w <= 0 || h <= 0) return;

            // 1. Deep Aegean gradient
            bgPaint.setShader(new LinearGradient(
                    0, 0, w * 0.25f, h,
                    new int[]{COLOR_BG_DEEP, COLOR_BG_MID, COLOR_BG_HORIZON, COLOR_BG_DEEP},
                    new float[]{0f, 0.42f, 0.78f, 1f},
                    Shader.TileMode.CLAMP));
            c.drawRect(b, bgPaint);

            // 2. Subtle atmospheric glow behind upper-left emblem area
            glowPaint.setShader(new RadialGradient(
                    w * 0.26f, h * 0.34f, Math.max(w, h) * 0.48f,
                    new int[]{0x442878A8, 0x181B567C, 0x00071626},
                    new float[]{0f, 0.5f, 1f},
                    Shader.TileMode.CLAMP));
            c.drawRect(b, glowPaint);

            // 3. Subtle warm golden glow behind right action panel
            glowPaint.setShader(new RadialGradient(
                    w * 0.78f, h * 0.55f, Math.max(w, h) * 0.35f,
                    new int[]{0x22D4A446, 0x00071626},
                    new float[]{0f, 1f},
                    Shader.TileMode.CLAMP));
            c.drawRect(b, glowPaint);

            // 4. Nautical compass rose watermark
            float cx = w * 0.52f;
            float cy = h * 0.52f;
            float rOuter = Math.min(w, h) * 0.46f;
            linePaint.setColor(0x16F2CD72);
            linePaint.setStrokeWidth(1.2f * density);
            c.drawCircle(cx, cy, rOuter, linePaint);
            c.drawCircle(cx, cy, rOuter * 0.86f, linePaint);
            c.drawCircle(cx, cy, rOuter * 0.45f, linePaint);

            for (int deg = 0; deg < 360; deg += 30) {
                double rad = Math.toRadians(deg);
                float r1 = (deg % 90 == 0) ? rOuter * 0.22f : rOuter * 0.78f;
                float r2 = (deg % 90 == 0) ? rOuter * 1.06f : rOuter * 0.94f;
                c.drawLine(
                        cx + (float) Math.cos(rad) * r1,
                        cy + (float) Math.sin(rad) * r1,
                        cx + (float) Math.cos(rad) * r2,
                        cy + (float) Math.sin(rad) * r2,
                        linePaint);
            }

            // 5. Top & bottom Arcadian gold trim lines with Greek meander motif
            drawMeanderStrip(c, w, 10f * density);
            drawMeanderStrip(c, w, h - 18f * density);
        }

        private void drawMeanderStrip(Canvas c, float w, float y) {
            float margin = 20f * density;
            float stripH = 8f * density;
            goldPaint.setColor(0x55D4A446);
            goldPaint.setStrokeWidth(1f * density);
            c.drawLine(margin, y, w - margin, y, goldPaint);
            c.drawLine(margin, y + stripH, w - margin, y + stripH, goldPaint);

            // Subtle geometric Greek key steps
            goldPaint.setColor(0x3CD4A446);
            float step = 16f * density;
            float x = margin + 4f * density;
            Path p = new Path();
            while (x + step < w - margin - 4f * density) {
                p.moveTo(x, y + stripH - 1.5f * density);
                p.lineTo(x, y + 2f * density);
                p.lineTo(x + step * 0.62f, y + 2f * density);
                p.lineTo(x + step * 0.62f, y + stripH - 2.5f * density);
                p.lineTo(x + step * 0.30f, y + stripH - 2.5f * density);
                p.lineTo(x + step * 0.30f, y + stripH * 0.48f);
                x += step;
            }
            c.drawPath(p, goldPaint);

            // Center diamond accent
            float cx = w * 0.5f;
            float cy = y + stripH * 0.5f;
            float d = 4.5f * density;
            Path dm = new Path();
            dm.moveTo(cx, cy - d);
            dm.lineTo(cx + d * 1.4f, cy);
            dm.lineTo(cx, cy + d);
            dm.lineTo(cx - d * 1.4f, cy);
            dm.close();
            Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
            fill.setColor(0xFFD4A446);
            c.drawPath(dm, fill);
        }

        @Override public void setAlpha(int alpha) {}
        @Override public void setColorFilter(ColorFilter cf) {}
        @Override public int getOpacity() { return PixelFormat.OPAQUE; }
    }

    /** Translucent dark glass card with beveled antique gold border & optional corner diamond accents. */
    public static final class GoldCardDrawable extends Drawable {
        private final float density;
        private final int fillColor;
        private final int borderColor;
        private final float radius;
        private final boolean showDiamonds;
        private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint borderPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint accentPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF rect = new RectF();

        public GoldCardDrawable(Context ctx, int fillColor, int borderColor, float radiusDp) {
            this(ctx, fillColor, borderColor, radiusDp, true);
        }

        public GoldCardDrawable(Context ctx, int fillColor, int borderColor, float radiusDp, boolean showDiamonds) {
            this.density = ctx.getResources().getDisplayMetrics().density;
            this.fillColor = fillColor;
            this.borderColor = borderColor;
            this.radius = radiusDp * density;
            this.showDiamonds = showDiamonds;
            fillPaint.setStyle(Paint.Style.FILL);
            fillPaint.setColor(fillColor);
            borderPaint.setStyle(Paint.Style.STROKE);
            borderPaint.setStrokeWidth(1.35f * density);
            borderPaint.setColor(borderColor);
            accentPaint.setStyle(Paint.Style.FILL);
            accentPaint.setColor(COLOR_GOLD_BRIGHT);
        }

        @Override
        public void draw(Canvas c) {
            Rect b = getBounds();
            float inset = 1.5f * density;
            rect.set(b.left + inset, b.top + inset, b.right - inset, b.bottom - inset);
            c.drawRoundRect(rect, radius, radius, fillPaint);
            c.drawRoundRect(rect, radius, radius, borderPaint);

            // Subtle inner highlight at top edge
            Paint hi = new Paint(Paint.ANTI_ALIAS_FLAG);
            hi.setColor(0x28FFF0C2);
            hi.setStrokeWidth(1f * density);
            c.drawLine(rect.left + radius, rect.top + density, rect.right - radius, rect.top + density, hi);

            if (showDiamonds) {
                float d = 2.6f * density;
                float o = 8f * density;
                drawDiamond(c, rect.left + o, rect.top + o, d);
                drawDiamond(c, rect.right - o, rect.top + o, d);
                drawDiamond(c, rect.left + o, rect.bottom - o, d);
                drawDiamond(c, rect.right - o, rect.bottom - o, d);
            }
        }

        private void drawDiamond(Canvas c, float cx, float cy, float d) {
            Path p = new Path();
            p.moveTo(cx, cy - d);
            p.lineTo(cx + d, cy);
            p.lineTo(cx, cy + d);
            p.lineTo(cx - d, cy);
            p.close();
            accentPaint.setAlpha(150);
            c.drawPath(p, accentPaint);
        }

        @Override public void setAlpha(int alpha) {}
        @Override public void setColorFilter(ColorFilter cf) {}
        @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    }

    /** Stylized gradient CTA button with double gold border and press feedback. */
    public static final class ArcadianButtonDrawable extends Drawable {
        private final float density;
        private final int topColor;
        private final int botColor;
        private final int strokeColor;
        private boolean pressed;

        private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint strokePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint innerPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF rect = new RectF();

        public ArcadianButtonDrawable(Context ctx, int topColor, int botColor, int strokeColor) {
            this.density = ctx.getResources().getDisplayMetrics().density;
            this.topColor = topColor;
            this.botColor = botColor;
            this.strokeColor = strokeColor;
            strokePaint.setStyle(Paint.Style.STROKE);
            strokePaint.setStrokeWidth(1.6f * density);
            strokePaint.setColor(strokeColor);
            innerPaint.setStyle(Paint.Style.STROKE);
            innerPaint.setStrokeWidth(0.9f * density);
            innerPaint.setColor(0x44FFF4D0);
        }

        @Override
        public boolean isStateful() {
            return true;
        }

        @Override
        protected boolean onStateChange(int[] state) {
            boolean p = false;
            for (int s : state) {
                if (s == android.R.attr.state_pressed) {
                    p = true;
                    break;
                }
            }
            if (p != pressed) {
                pressed = p;
                invalidateSelf();
                return true;
            }
            return false;
        }

        @Override
        public void draw(Canvas c) {
            Rect b = getBounds();
            if (b.width() <= 0 || b.height() <= 0) return;
            float inset = 1.5f * density;
            float r = 10f * density;
            rect.set(b.left + inset, b.top + inset, b.right - inset, b.bottom - inset);

            int c1 = pressed ? botColor : topColor;
            int c2 = pressed ? topColor : botColor;
            fillPaint.setShader(new LinearGradient(0, rect.top, 0, rect.bottom, c1, c2, Shader.TileMode.CLAMP));
            c.drawRoundRect(rect, r, r, fillPaint);
            c.drawRoundRect(rect, r, r, strokePaint);

            RectF inner = new RectF(rect.left + 3f * density, rect.top + 3f * density,
                    rect.right - 3f * density, rect.bottom - 3f * density);
            c.drawRoundRect(inner, Math.max(2f, r - 2f * density), Math.max(2f, r - 2f * density), innerPaint);
        }

        @Override public void setAlpha(int alpha) {}
        @Override public void setColorFilter(ColorFilter cf) {}
        @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    }

    /** Stylized Arcadian Emblem Crest view (golden compass diamond + nested Arcadian shield). */
    public static final class EmblemView extends View {
        private final float density;
        private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint strokePaint = new Paint(Paint.ANTI_ALIAS_FLAG);

        public EmblemView(Context ctx) {
            super(ctx);
            this.density = ctx.getResources().getDisplayMetrics().density;
            strokePaint.setStyle(Paint.Style.STROKE);
        }

        @Override
        protected void onDraw(Canvas c) {
            float w = getWidth();
            float h = getHeight();
            float cx = w * 0.5f;
            float cy = h * 0.5f;
            float r = Math.min(w, h) * 0.44f;

            // Outer golden ring
            strokePaint.setColor(COLOR_GOLD_PRIMARY);
            strokePaint.setStrokeWidth(2.2f * density);
            c.drawCircle(cx, cy, r, strokePaint);

            // Deep sapphire inner disc
            fillPaint.setShader(new RadialGradient(cx, cy - r * 0.25f, r,
                    new int[]{0xFF1D5885, 0xFF0A2036}, null, Shader.TileMode.CLAMP));
            c.drawCircle(cx, cy, r - 2.2f * density, fillPaint);
            fillPaint.setShader(null);

            // Inner thin ring
            strokePaint.setColor(0x99F4D17A);
            strokePaint.setStrokeWidth(1f * density);
            c.drawCircle(cx, cy, r * 0.80f, strokePaint);

            // Stylized Arcadian Triforce-inspired / Knight Shield emblem
            Path tri = new Path();
            float tr = r * 0.56f;
            tri.moveTo(cx, cy - tr * 0.95f);
            tri.lineTo(cx + tr * 0.88f, cy + tr * 0.65f);
            tri.lineTo(cx - tr * 0.88f, cy + tr * 0.65f);
            tri.close();

            fillPaint.setShader(new LinearGradient(cx, cy - tr, cx, cy + tr,
                    COLOR_GOLD_BRIGHT, COLOR_GOLD_PRIMARY, Shader.TileMode.CLAMP));
            c.drawPath(tri, fillPaint);
            fillPaint.setShader(null);

            // Inverted inner triangle cut-out
            Path innerTri = new Path();
            float itr = tr * 0.44f;
            innerTri.moveTo(cx, cy + itr * 0.85f);
            innerTri.lineTo(cx + itr * 0.86f, cy - itr * 0.35f);
            innerTri.lineTo(cx - itr * 0.86f, cy - itr * 0.35f);
            innerTri.close();
            fillPaint.setColor(0xFF0D2B45);
            c.drawPath(innerTri, fillPaint);

            // 4 tiny cardinal gold diamonds on outer ring
            fillPaint.setColor(COLOR_GOLD_BRIGHT);
            float d = 3.2f * density;
            drawDiamond(c, cx, cy - r, d);
            drawDiamond(c, cx, cy + r, d);
            drawDiamond(c, cx - r, cy, d);
            drawDiamond(c, cx + r, cy, d);
        }

        private void drawDiamond(Canvas c, float x, float y, float d) {
            Path p = new Path();
            p.moveTo(x, y - d);
            p.lineTo(x + d, y);
            p.lineTo(x, y + d);
            p.lineTo(x - d, y);
            p.close();
            c.drawPath(p, fillPaint);
        }
    }

    /** Arcadian diamond status badge (OK / WARN / ERR). */
    public static final class StatusBadgeView extends View {
        public static final int STATE_WARN = 0;
        public static final int STATE_OK   = 1;
        public static final int STATE_ERR  = 2;

        private int state = STATE_WARN;
        private final float density;
        private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint strokePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint iconPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

        public StatusBadgeView(Context ctx) {
            super(ctx);
            this.density = ctx.getResources().getDisplayMetrics().density;
            strokePaint.setStyle(Paint.Style.STROKE);
            strokePaint.setStrokeWidth(1.5f * density);
            iconPaint.setStyle(Paint.Style.STROKE);
            iconPaint.setStrokeWidth(2.1f * density);
            iconPaint.setStrokeCap(Paint.Cap.ROUND);
            iconPaint.setStrokeJoin(Paint.Join.ROUND);
        }

        public void setStatusState(int s) {
            if (this.state != s) {
                this.state = s;
                invalidate();
            }
        }

        @Override
        protected void onDraw(Canvas c) {
            float cx = getWidth() * 0.5f;
            float cy = getHeight() * 0.5f;
            float r = Math.min(cx, cy) - 2f * density;

            int accent = (state == STATE_OK) ? COLOR_OK : (state == STATE_ERR ? COLOR_ERR : COLOR_WARN);
            Path dm = new Path();
            dm.moveTo(cx, cy - r);
            dm.lineTo(cx + r, cy);
            dm.lineTo(cx, cy + r);
            dm.lineTo(cx - r, cy);
            dm.close();

            fillPaint.setColor((accent & 0x00FFFFFF) | 0x26000000);
            strokePaint.setColor(accent);
            c.drawPath(dm, fillPaint);
            c.drawPath(dm, strokePaint);

            iconPaint.setColor(accent);
            if (state == STATE_OK) {
                Path chk = new Path();
                chk.moveTo(cx - r * 0.36f, cy + r * 0.02f);
                chk.lineTo(cx - r * 0.08f, cy + r * 0.30f);
                chk.lineTo(cx + r * 0.38f, cy - r * 0.24f);
                c.drawPath(chk, iconPaint);
            } else if (state == STATE_ERR) {
                float d = r * 0.28f;
                c.drawLine(cx - d, cy - d, cx + d, cy + d, iconPaint);
                c.drawLine(cx + d, cy - d, cx - d, cy + d, iconPaint);
            } else {
                c.drawCircle(cx, cy, 2.2f * density, strokePaint);
            }
        }
    }

    /** Custom golden progress bar with beveled track. */
    public static final class GoldProgressBar extends View {
        private int progress = 0;
        private int max = 100;
        private final float density;
        private final Paint trackPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint borderPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF trackRect = new RectF();
        private final RectF fillRect = new RectF();

        public GoldProgressBar(Context ctx) {
            super(ctx);
            this.density = ctx.getResources().getDisplayMetrics().density;
            trackPaint.setColor(0xFF071522);
            borderPaint.setStyle(Paint.Style.STROKE);
            borderPaint.setStrokeWidth(1.2f * density);
            borderPaint.setColor(COLOR_GOLD_DIM);
        }

        public void setProgress(int progress, int max) {
            this.max = Math.max(1, max);
            this.progress = Math.max(0, Math.min(progress, this.max));
            invalidate();
        }

        @Override
        protected void onDraw(Canvas c) {
            float w = getWidth();
            float h = getHeight();
            float r = h * 0.5f;
            float inset = 1f * density;
            trackRect.set(inset, inset, w - inset, h - inset);
            c.drawRoundRect(trackRect, r, r, trackPaint);

            float frac = (float) progress / (float) max;
            float fillW = Math.max(0f, (w - 4f * density) * frac);
            if (fillW > 2f * density) {
                fillRect.set(2f * density, 2f * density, 2f * density + fillW, h - 2f * density);
                fillPaint.setShader(new LinearGradient(
                        fillRect.left, 0, fillRect.right, 0,
                        COLOR_GOLD_PRIMARY, COLOR_GOLD_BRIGHT, Shader.TileMode.CLAMP));
                c.drawRoundRect(fillRect, r, r, fillPaint);
            }
            c.drawRoundRect(trackRect, r, r, borderPaint);
        }
    }
}
