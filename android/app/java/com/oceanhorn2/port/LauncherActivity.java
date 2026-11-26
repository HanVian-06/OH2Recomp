package com.oceanhorn2.port;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Standalone Arcadian-themed Launcher Activity for Oceanhorn 2 Android Port.
 * Lets users import their own decrypted Oceanhorn 2 .IPA, compiles all 4,544 AIR 2.0
 * shaders to Vulkan 1.1 SPIR-V 1.3 directly on-device, and launches NativeActivity.
 */
public final class LauncherActivity extends Activity {

    private static final int REQ_PICK_IPA = 1001;

    private final Handler uiHandler = new Handler(Looper.getMainLooper());
    private final AtomicBoolean importCancelled = new AtomicBoolean(false);
    private boolean isBusy = false;

    private File filesDir;

    // Header / Device info
    private TextView deviceInfoView;

    // Checklist rows
    private ArcadianViews.StatusBadgeView badgeOh2;
    private TextView subOh2;
    private ArcadianViews.StatusBadgeView badgePak;
    private TextView subPak;
    private ArcadianViews.StatusBadgeView badgeSpirv;
    private TextView subSpirv;

    // Action buttons
    private TextView btnPlay;
    private TextView btnImport;
    private TextView btnRebuildShaders;
    private TextView btnClearData;
    private TextView statusBanner;

    // Progress overlay
    private FrameLayout progressOverlay;
    private TextView progressStepTag;
    private TextView progressTitle;
    private TextView progressDetail;
    private TextView progressPct;
    private ArcadianViews.GoldProgressBar progressBar;
    private TextView btnCancelImport;

    private boolean readyToPlay = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN
                | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        applyImmersiveFullscreen();

        filesDir = getExternalFilesDir(null);
        if (filesDir == null) {
            filesDir = getFilesDir();
        }

        setContentView(buildUi());
        queryDeviceInfoAsync();
        refreshStatus();
    }

    @Override
    protected void onResume() {
        super.onResume();
        applyImmersiveFullscreen();
        if (!isBusy) {
            refreshStatus();
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            applyImmersiveFullscreen();
        }
    }

    private void applyImmersiveFullscreen() {
        View decor = getWindow().getDecorView();
        int flags = View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY;
        decor.setSystemUiVisibility(flags);
        if (Build.VERSION.SDK_INT >= 28) {
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(lp);
        }
    }

    private View buildUi() {
        FrameLayout root = new FrameLayout(this);
        root.setBackground(new ArcadianViews.BackdropDrawable(this));

        int padH = ArcadianViews.dp(this, 36);
        int padV = ArcadianViews.dp(this, 26);

        LinearLayout mainCols = new LinearLayout(this);
        mainCols.setOrientation(LinearLayout.HORIZONTAL);
        mainCols.setGravity(Gravity.CENTER_VERTICAL);
        mainCols.setPadding(padH, padV, padH, padV);

        // ==================== LEFT COLUMN: Brand + Checklist ====================
        LinearLayout leftCol = new LinearLayout(this);
        leftCol.setOrientation(LinearLayout.VERTICAL);
        LinearLayout.LayoutParams leftLp = new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.18f);
        leftLp.rightMargin = ArcadianViews.dp(this, 24);
        leftCol.setLayoutParams(leftLp);

        // Header row: Emblem + Title
        LinearLayout headerRow = new LinearLayout(this);
        headerRow.setOrientation(LinearLayout.HORIZONTAL);
        headerRow.setGravity(Gravity.CENTER_VERTICAL);

        ArcadianViews.EmblemView emblem = new ArcadianViews.EmblemView(this);
        int embSz = ArcadianViews.dp(this, 62);
        LinearLayout.LayoutParams embLp = new LinearLayout.LayoutParams(embSz, embSz);
        embLp.rightMargin = ArcadianViews.dp(this, 16);
        emblem.setLayoutParams(embLp);
        headerRow.addView(emblem);

        LinearLayout titleBox = new LinearLayout(this);
        titleBox.setOrientation(LinearLayout.VERTICAL);
        titleBox.setLayoutParams(new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

        TextView titleTv = new TextView(this);
        titleTv.setText("OCEANHORN 2");
        titleTv.setTextColor(ArcadianViews.COLOR_GOLD_BRIGHT);
        titleTv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 25);
        titleTv.setTypeface(Typeface.SERIF, Typeface.BOLD);
        titleTv.setLetterSpacing(0.08f);
        titleTv.setSingleLine(true);
        titleTv.setShadowLayer(8f, 0f, 3f, 0xCC000000);
        titleBox.addView(titleTv);

        TextView subtitleTv = new TextView(this);
        subtitleTv.setText("KNIGHTS OF THE LOST REALM  •  ANDROID VULKAN");
        subtitleTv.setTextColor(ArcadianViews.COLOR_TEXT_MUTED);
        subtitleTv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 9.5f);
        subtitleTv.setTypeface(Typeface.SANS_SERIF, Typeface.BOLD);
        subtitleTv.setLetterSpacing(0.08f);
        subtitleTv.setSingleLine(true);
        titleBox.addView(subtitleTv);

        deviceInfoView = new TextView(this);
        deviceInfoView.setText("Detecting Vulkan GPU...");
        deviceInfoView.setTextColor(ArcadianViews.COLOR_GOLD_PRIMARY);
        deviceInfoView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f);
        LinearLayout.LayoutParams devLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        devLp.topMargin = ArcadianViews.dp(this, 4);
        deviceInfoView.setLayoutParams(devLp);
        titleBox.addView(deviceInfoView);

        headerRow.addView(titleBox);
        leftCol.addView(headerRow);

        // Checklist Card
        LinearLayout checkCard = new LinearLayout(this);
        checkCard.setOrientation(LinearLayout.VERTICAL);
        checkCard.setBackground(new ArcadianViews.GoldCardDrawable(
                this, ArcadianViews.COLOR_CARD_BG, ArcadianViews.COLOR_GOLD_DIM, 12f));
        int cardPad = ArcadianViews.dp(this, 16);
        checkCard.setPadding(cardPad, ArcadianViews.dp(this, 13), cardPad, ArcadianViews.dp(this, 13));
        LinearLayout.LayoutParams cardLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        cardLp.topMargin = ArcadianViews.dp(this, 16);
        checkCard.setLayoutParams(cardLp);

        TextView checkHeader = new TextView(this);
        checkHeader.setText("INSTALLATION STATUS");
        checkHeader.setTextColor(ArcadianViews.COLOR_GOLD_PRIMARY);
        checkHeader.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10.5f);
        checkHeader.setTypeface(Typeface.SANS_SERIF, Typeface.BOLD);
        checkHeader.setLetterSpacing(0.14f);
        checkCard.addView(checkHeader);

        badgeOh2 = new ArcadianViews.StatusBadgeView(this);
        subOh2 = new TextView(this);
        checkCard.addView(buildChecklistRow("ARM64 Game Binary (OH2)", badgeOh2, subOh2));

        checkCard.addView(buildDivider());

        badgePak = new ArcadianViews.StatusBadgeView(this);
        subPak = new TextView(this);
        checkCard.addView(buildChecklistRow("Game Assets (oh2-ios.pak)", badgePak, subPak));

        checkCard.addView(buildDivider());

        badgeSpirv = new ArcadianViews.StatusBadgeView(this);
        subSpirv = new TextView(this);
        checkCard.addView(buildChecklistRow("Vulkan SPIR-V Shaders", badgeSpirv, subSpirv));

        leftCol.addView(checkCard);
        mainCols.addView(leftCol);

        // ==================== RIGHT COLUMN: Action Panel ====================
        LinearLayout rightCol = new LinearLayout(this);
        rightCol.setOrientation(LinearLayout.VERTICAL);
        rightCol.setBackground(new ArcadianViews.GoldCardDrawable(
                this, ArcadianViews.COLOR_CARD_BG, ArcadianViews.COLOR_GOLD_PRIMARY, 14f));
        int rPad = ArcadianViews.dp(this, 20);
        rightCol.setPadding(rPad, ArcadianViews.dp(this, 18), rPad, ArcadianViews.dp(this, 16));
        LinearLayout.LayoutParams rightLp = new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 0.86f);
        rightCol.setLayoutParams(rightLp);

        TextView actionTitle = new TextView(this);
        actionTitle.setText("LAUNCH CONTROL");
        actionTitle.setTextColor(ArcadianViews.COLOR_GOLD_PRIMARY);
        actionTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10.5f);
        actionTitle.setTypeface(Typeface.SANS_SERIF, Typeface.BOLD);
        actionTitle.setLetterSpacing(0.14f);
        actionTitle.setGravity(Gravity.CENTER_HORIZONTAL);
        rightCol.addView(actionTitle);

        statusBanner = new TextView(this);
        statusBanner.setTextColor(ArcadianViews.COLOR_TEXT_MUTED);
        statusBanner.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        statusBanner.setGravity(Gravity.CENTER_HORIZONTAL);
        LinearLayout.LayoutParams sbLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        sbLp.topMargin = ArcadianViews.dp(this, 6);
        sbLp.bottomMargin = ArcadianViews.dp(this, 14);
        statusBanner.setLayoutParams(sbLp);
        rightCol.addView(statusBanner);

        // PLAY button
        btnPlay = buildStyledButton("PLAY OCEANHORN 2", 16f, true);
        btnPlay.setOnClickListener(v -> launchGame());
        rightCol.addView(btnPlay);

        // IMPORT .IPA button
        btnImport = buildStyledButton("IMPORT .IPA", 14f, false);
        LinearLayout.LayoutParams impLp = (LinearLayout.LayoutParams) btnImport.getLayoutParams();
        impLp.topMargin = ArcadianViews.dp(this, 10);
        btnImport.setLayoutParams(impLp);
        btnImport.setOnClickListener(v -> pickIpaFile());
        rightCol.addView(btnImport);

        // Utility row: Rebuild Shaders | Clear | About
        LinearLayout utilRow = new LinearLayout(this);
        utilRow.setOrientation(LinearLayout.HORIZONTAL);
        utilRow.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams uLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        uLp.topMargin = ArcadianViews.dp(this, 12);
        utilRow.setLayoutParams(uLp);

        btnRebuildShaders = buildSmallUtilityButton("SHADERS");
        btnRebuildShaders.setOnClickListener(v -> startShaderRebuild());
        utilRow.addView(btnRebuildShaders);

        btnClearData = buildSmallUtilityButton("CLEAR DATA");
        btnClearData.setOnClickListener(v -> confirmClearData());
        utilRow.addView(btnClearData);

        TextView btnAbout = buildSmallUtilityButton("ABOUT");
        btnAbout.setOnClickListener(v -> showAboutDialog());
        utilRow.addView(btnAbout);

        rightCol.addView(utilRow);

        TextView footerNote = new TextView(this);
        footerNote.setText("Open-Source MIT  •  User-supplied decrypted .IPA required");
        footerNote.setTextColor(0x88A5BCCE);
        footerNote.setTextSize(TypedValue.COMPLEX_UNIT_SP, 9.5f);
        footerNote.setGravity(Gravity.CENTER_HORIZONTAL);
        LinearLayout.LayoutParams fnLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        fnLp.topMargin = ArcadianViews.dp(this, 10);
        footerNote.setLayoutParams(fnLp);
        rightCol.addView(footerNote);

        mainCols.addView(rightCol);
        root.addView(mainCols, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));

        // ==================== PROGRESS MODAL OVERLAY ====================
        progressOverlay = buildProgressOverlay();
        progressOverlay.setVisibility(View.GONE);
        root.addView(progressOverlay, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));

        return root;
    }

    private LinearLayout buildChecklistRow(String title, ArcadianViews.StatusBadgeView badge, TextView sub) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        int pv = ArcadianViews.dp(this, 8);
        row.setPadding(0, pv, 0, pv);

        int bSz = ArcadianViews.dp(this, 26);
        LinearLayout.LayoutParams bLp = new LinearLayout.LayoutParams(bSz, bSz);
        bLp.rightMargin = ArcadianViews.dp(this, 12);
        badge.setLayoutParams(bLp);
        row.addView(badge);

        LinearLayout texts = new LinearLayout(this);
        texts.setOrientation(LinearLayout.VERTICAL);

        TextView titleTv = new TextView(this);
        titleTv.setText(title);
        titleTv.setTextColor(ArcadianViews.COLOR_TEXT_PRIMARY);
        titleTv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13.5f);
        titleTv.setTypeface(Typeface.SANS_SERIF, Typeface.BOLD);
        texts.addView(titleTv);

        sub.setTextColor(ArcadianViews.COLOR_TEXT_MUTED);
        sub.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f);
        texts.addView(sub);

        row.addView(texts);
        return row;
    }

    private View buildDivider() {
        View v = new View(this);
        v.setBackgroundColor(0x2AD4A446);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, Math.max(1, ArcadianViews.dp(this, 1)));
        v.setLayoutParams(lp);
        return v;
    }

    private TextView buildStyledButton(String label, float textSp, boolean primary) {
        TextView tv = new TextView(this);
        tv.setText(label);
        tv.setTextColor(ArcadianViews.COLOR_TEXT_PRIMARY);
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, textSp);
        tv.setTypeface(Typeface.SERIF, Typeface.BOLD);
        tv.setLetterSpacing(0.08f);
        tv.setGravity(Gravity.CENTER);
        tv.setShadowLayer(4f, 0f, 2f, 0xAA000000);
        int pv = ArcadianViews.dp(this, primary ? 14 : 11);
        tv.setPadding(ArcadianViews.dp(this, 16), pv, ArcadianViews.dp(this, 16), pv);
        tv.setClickable(true);
        tv.setFocusable(true);
        tv.setBackground(new ArcadianViews.ArcadianButtonDrawable(
                this,
                primary ? ArcadianViews.COLOR_CTA_TOP : ArcadianViews.COLOR_SEC_TOP,
                primary ? ArcadianViews.COLOR_CTA_BOT : ArcadianViews.COLOR_SEC_BOT,
                primary ? ArcadianViews.COLOR_GOLD_BRIGHT : ArcadianViews.COLOR_GOLD_PRIMARY));
        tv.setLayoutParams(new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        return tv;
    }

    private TextView buildSmallUtilityButton(String label) {
        TextView tv = new TextView(this);
        tv.setText(label);
        tv.setTextColor(ArcadianViews.COLOR_GOLD_BRIGHT);
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10f);
        tv.setTypeface(Typeface.SANS_SERIF, Typeface.BOLD);
        tv.setLetterSpacing(0.06f);
        tv.setGravity(Gravity.CENTER);
        tv.setPadding(ArcadianViews.dp(this, 8), ArcadianViews.dp(this, 7),
                ArcadianViews.dp(this, 8), ArcadianViews.dp(this, 7));
        tv.setClickable(true);
        tv.setFocusable(true);
        tv.setBackground(new ArcadianViews.GoldCardDrawable(
                this, ArcadianViews.COLOR_CARD_INNER, 0x66D4A446, 7f, false));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        int m = ArcadianViews.dp(this, 3);
        lp.leftMargin = m;
        lp.rightMargin = m;
        tv.setLayoutParams(lp);
        return tv;
    }

    private FrameLayout buildProgressOverlay() {
        FrameLayout overlay = new FrameLayout(this);
        overlay.setBackgroundColor(0xCC040D17);
        overlay.setClickable(true);
        overlay.setFocusable(true);

        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setGravity(Gravity.CENTER_HORIZONTAL);
        card.setBackground(new ArcadianViews.GoldCardDrawable(
                this, 0xF20B1E30, ArcadianViews.COLOR_GOLD_BRIGHT, 14f));
        int p = ArcadianViews.dp(this, 24);
        card.setPadding(p, ArcadianViews.dp(this, 20), p, ArcadianViews.dp(this, 18));

        progressStepTag = new TextView(this);
        progressStepTag.setText("STEP 1 OF 3");
        progressStepTag.setTextColor(ArcadianViews.COLOR_GOLD_PRIMARY);
        progressStepTag.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f);
        progressStepTag.setTypeface(Typeface.SANS_SERIF, Typeface.BOLD);
        progressStepTag.setLetterSpacing(0.16f);
        card.addView(progressStepTag);

        progressTitle = new TextView(this);
        progressTitle.setText("Preparing Oceanhorn 2...");
        progressTitle.setTextColor(ArcadianViews.COLOR_TEXT_PRIMARY);
        progressTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 19f);
        progressTitle.setTypeface(Typeface.SERIF, Typeface.BOLD);
        LinearLayout.LayoutParams ptLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        ptLp.topMargin = ArcadianViews.dp(this, 4);
        progressTitle.setLayoutParams(ptLp);
        card.addView(progressTitle);

        progressDetail = new TextView(this);
        progressDetail.setText("Starting...");
        progressDetail.setTextColor(ArcadianViews.COLOR_TEXT_MUTED);
        progressDetail.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f);
        LinearLayout.LayoutParams pdLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        pdLp.topMargin = ArcadianViews.dp(this, 6);
        pdLp.bottomMargin = ArcadianViews.dp(this, 14);
        progressDetail.setLayoutParams(pdLp);
        card.addView(progressDetail);

        progressBar = new ArcadianViews.GoldProgressBar(this);
        LinearLayout.LayoutParams pbLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, ArcadianViews.dp(this, 14));
        progressBar.setLayoutParams(pbLp);
        card.addView(progressBar);

        progressPct = new TextView(this);
        progressPct.setText("0%");
        progressPct.setTextColor(ArcadianViews.COLOR_GOLD_BRIGHT);
        progressPct.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        progressPct.setTypeface(Typeface.SANS_SERIF, Typeface.BOLD);
        LinearLayout.LayoutParams ppLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        ppLp.topMargin = ArcadianViews.dp(this, 6);
        progressPct.setLayoutParams(ppLp);
        card.addView(progressPct);

        btnCancelImport = buildSmallUtilityButton("CANCEL");
        LinearLayout.LayoutParams cLp = new LinearLayout.LayoutParams(
                ArcadianViews.dp(this, 120), LinearLayout.LayoutParams.WRAP_CONTENT);
        cLp.topMargin = ArcadianViews.dp(this, 14);
        btnCancelImport.setLayoutParams(cLp);
        btnCancelImport.setOnClickListener(v -> {
            importCancelled.set(true);
            btnCancelImport.setEnabled(false);
            btnCancelImport.setText("CANCELLING...");
        });
        card.addView(btnCancelImport);

        FrameLayout.LayoutParams cardLp = new FrameLayout.LayoutParams(
                ArcadianViews.dp(this, 440), LinearLayout.LayoutParams.WRAP_CONTENT, Gravity.CENTER);
        overlay.addView(card, cardLp);
        return overlay;
    }

    private void queryDeviceInfoAsync() {
        new Thread(() -> {
            String info;
            try {
                info = NativeBridge.nativeGetVulkanDeviceInfo();
            } catch (Throwable t) {
                info = "Vulkan 1.1+ Runtime";
            }
            final String finalInfo = info;
            uiHandler.post(() -> deviceInfoView.setText(finalInfo));
        }, "OH2-VkInfo").start();
    }

    private boolean hasBundledAssetMachO() {
        try {
            String[] list = getAssets().list("");
            if (list != null) {
                for (String s : list) {
                    if ("oh2_macho.bin".equals(s)) return true;
                }
            }
        } catch (Throwable ignored) {}
        return false;
    }

    private void refreshStatus() {
        File oh2File = new File(filesDir, IpaImporter.REL_OH2_BIN);
        File pakFile = new File(filesDir, IpaImporter.REL_PAK);
        File spvFile = new File(filesDir, IpaImporter.REL_SPIRV);

        boolean oh2Ok = false;
        if (oh2File.isFile()) {
            int st = NativeBridge.nativeVerifyMachO(oh2File.getAbsolutePath());
            if (st == 0) {
                oh2Ok = true;
                badgeOh2.setStatusState(ArcadianViews.StatusBadgeView.STATE_OK);
                subOh2.setText(String.format(Locale.US, "Verified decrypted ARM64  •  %.1f MB",
                        oh2File.length() / (1024.0 * 1024.0)));
            } else if (st == 1) {
                badgeOh2.setStatusState(ArcadianViews.StatusBadgeView.STATE_ERR);
                subOh2.setText("Encrypted FairPlay binary (cryptid != 0)");
            } else {
                badgeOh2.setStatusState(ArcadianViews.StatusBadgeView.STATE_ERR);
                subOh2.setText("Invalid Mach-O file");
            }
        } else if (hasBundledAssetMachO()) {
            oh2Ok = true;
            badgeOh2.setStatusState(ArcadianViews.StatusBadgeView.STATE_OK);
            subOh2.setText("Dev build: bundled inside APK assets");
        } else {
            badgeOh2.setStatusState(ArcadianViews.StatusBadgeView.STATE_WARN);
            subOh2.setText("Not imported yet  •  Select a decrypted .IPA");
        }

        boolean pakOk = pakFile.isFile() && pakFile.length() > 100L * 1024L * 1024L;
        if (pakOk) {
            badgePak.setStatusState(ArcadianViews.StatusBadgeView.STATE_OK);
            subPak.setText(String.format(Locale.US, "Ready  •  %.2f GB",
                    pakFile.length() / (1024.0 * 1024.0 * 1024.0)));
        } else {
            badgePak.setStatusState(ArcadianViews.StatusBadgeView.STATE_WARN);
            subPak.setText("Missing oh2-ios.pak archive");
        }

        int shaderCount = spvFile.isFile() ? NativeBridge.nativeVerifySpirvCache(spvFile.getAbsolutePath()) : 0;
        boolean spvOk = shaderCount > 0;
        if (spvOk) {
            badgeSpirv.setStatusState(ArcadianViews.StatusBadgeView.STATE_OK);
            subSpirv.setText(String.format(Locale.US, "%d Vulkan 1.1 shaders cached  •  %.1f MB",
                    shaderCount, spvFile.length() / (1024.0 * 1024.0)));
        } else if (pakOk) {
            badgeSpirv.setStatusState(ArcadianViews.StatusBadgeView.STATE_WARN);
            subSpirv.setText("Not compiled yet  •  Tap 'REBUILD SHADERS'");
        } else {
            badgeSpirv.setStatusState(ArcadianViews.StatusBadgeView.STATE_WARN);
            subSpirv.setText("Compiled automatically on-device after .IPA import");
        }

        readyToPlay = oh2Ok && pakOk && spvOk;
        btnPlay.setEnabled(readyToPlay);
        btnPlay.setAlpha(readyToPlay ? 1.0f : 0.45f);
        btnPlay.setBackground(new ArcadianViews.ArcadianButtonDrawable(
                this,
                readyToPlay ? ArcadianViews.COLOR_CTA_TOP : ArcadianViews.COLOR_DIS_TOP,
                readyToPlay ? ArcadianViews.COLOR_CTA_BOT : ArcadianViews.COLOR_DIS_BOT,
                readyToPlay ? ArcadianViews.COLOR_GOLD_BRIGHT : 0x558E6A25));

        btnRebuildShaders.setEnabled(pakOk);
        btnRebuildShaders.setAlpha(pakOk ? 1.0f : 0.42f);

        boolean hasAnyData = oh2File.exists() || pakFile.exists() || spvFile.exists();
        btnClearData.setEnabled(hasAnyData);
        btnClearData.setAlpha(hasAnyData ? 1.0f : 0.42f);

        if (readyToPlay) {
            statusBanner.setText("All game files & Vulkan shaders are ready.");
            statusBanner.setTextColor(ArcadianViews.COLOR_OK);
        } else if (oh2Ok && pakOk) {
            statusBanner.setText("Game files found! Tap REBUILD SHADERS to compile on-device.");
            statusBanner.setTextColor(ArcadianViews.COLOR_WARN);
        } else {
            statusBanner.setText("Import your decrypted Oceanhorn 2 .IPA to begin.");
            statusBanner.setTextColor(ArcadianViews.COLOR_TEXT_MUTED);
        }
    }

    private void launchGame() {
        if (!readyToPlay || isBusy) return;
        Intent intent = new Intent();
        intent.setClassName(this, "android.app.NativeActivity");
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(intent);
    }

    private void pickIpaFile() {
        if (isBusy) return;
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        startActivityForResult(intent, REQ_PICK_IPA);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_PICK_IPA && resultCode == RESULT_OK && data != null && data.getData() != null) {
            startIpaImport(data.getData());
        }
    }

    private void startIpaImport(Uri uri) {
        isBusy = true;
        importCancelled.set(false);
        showProgressModal(true, "STEP 1 OF 3", "Extracting Oceanhorn 2 .IPA", "Opening archive...", 0, 100);

        IpaImporter.startIpaImport(this, uri, filesDir, importCancelled, new IpaImporter.Callback() {
            @Override
            public void onStep(int step, int totalSteps, int progress, int max, String headline, String detail) {
                uiHandler.post(() -> {
                    boolean canCancel = (step == 1);
                    showProgressModal(canCancel,
                            String.format(Locale.US, "STEP %d OF %d", step, totalSteps),
                            headline, detail, progress, max);
                });
            }

            @Override
            public void onComplete(String summary) {
                uiHandler.post(() -> {
                    isBusy = false;
                    progressOverlay.setVisibility(View.GONE);
                    refreshStatus();
                    Toast.makeText(LauncherActivity.this, "Ready to play! " + summary, Toast.LENGTH_LONG).show();
                });
            }

            @Override
            public void onError(String message) {
                uiHandler.post(() -> {
                    isBusy = false;
                    progressOverlay.setVisibility(View.GONE);
                    refreshStatus();
                    new AlertDialog.Builder(LauncherActivity.this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                            .setTitle("Import Failed")
                            .setMessage(message)
                            .setPositiveButton("OK", null)
                            .show();
                });
            }
        });
    }

    private void startShaderRebuild() {
        if (isBusy) return;
        isBusy = true;
        showProgressModal(false, "ON-DEVICE COMPILER", "Compiling Vulkan Shaders", "Starting...", 0, 100);

        IpaImporter.startShaderCompileOnly(filesDir, new IpaImporter.Callback() {
            @Override
            public void onStep(int step, int totalSteps, int progress, int max, String headline, String detail) {
                uiHandler.post(() -> showProgressModal(false, "ON-DEVICE COMPILER", headline, detail, progress, max));
            }

            @Override
            public void onComplete(String summary) {
                uiHandler.post(() -> {
                    isBusy = false;
                    progressOverlay.setVisibility(View.GONE);
                    refreshStatus();
                    Toast.makeText(LauncherActivity.this, "Shaders ready: " + summary, Toast.LENGTH_LONG).show();
                });
            }

            @Override
            public void onError(String message) {
                uiHandler.post(() -> {
                    isBusy = false;
                    progressOverlay.setVisibility(View.GONE);
                    refreshStatus();
                    new AlertDialog.Builder(LauncherActivity.this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                            .setTitle("Shader Compilation Error")
                            .setMessage(message)
                            .setPositiveButton("OK", null)
                            .show();
                });
            }
        });
    }

    private void showProgressModal(boolean canCancel, String stepTag, String title, String detail, int prog, int max) {
        progressOverlay.setVisibility(View.VISIBLE);
        progressStepTag.setText(stepTag);
        progressTitle.setText(title);
        progressDetail.setText(detail);
        progressBar.setProgress(prog, max);
        int pct = (max > 0) ? (int) Math.min(100, (prog * 100L) / max) : 0;
        progressPct.setText(pct + "%");
        btnCancelImport.setVisibility(canCancel ? View.VISIBLE : View.GONE);
        if (canCancel && !importCancelled.get()) {
            btnCancelImport.setEnabled(true);
            btnCancelImport.setText("CANCEL");
        }
    }

    private void confirmClearData() {
        if (isBusy) return;
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Clear Imported Game Data?")
                .setMessage("This removes OH2, cookeddata, and spirv_cache.bin.\nYour save files in Documents/ will be preserved.")
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Clear", (d, w) -> {
                    deleteRecursive(new File(filesDir, IpaImporter.REL_OH2_BIN));
                    deleteRecursive(new File(filesDir, "cookeddata"));
                    deleteRecursive(new File(filesDir, "Engine"));
                    deleteRecursive(new File(filesDir, IpaImporter.REL_SPIRV));
                    deleteRecursive(new File(filesDir, "Info.plist"));
                    deleteRecursive(new File(filesDir, "ue4commandline.txt"));
                    refreshStatus();
                    Toast.makeText(this, "Game data cleared.", Toast.LENGTH_SHORT).show();
                })
                .show();
    }

    private void deleteRecursive(File f) {
        if (f == null || !f.exists()) return;
        if (f.isDirectory()) {
            File[] children = f.listFiles();
            if (children != null) {
                for (File c : children) deleteRecursive(c);
            }
        }
        f.delete();
    }

    private void showAboutDialog() {
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Oceanhorn 2 Android Runtime")
                .setMessage("Open-Source iOS ARM64 (Mach-O / Metal 2.0) to Android Vulkan 1.1 compatibility runtime.\n\n"
                        + "• Zero copyrighted game assets are bundled with this app.\n"
                        + "• Import your own legally owned, decrypted Oceanhorn 2 .IPA.\n"
                        + "• All 4,544 AIR 2.0 shaders are compiled to Vulkan 1.1 SPIR-V 1.3 directly on-device.\n\n"
                        + "License: MIT Open-Source")
                .setPositiveButton("Close", null)
                .show();
    }
}
