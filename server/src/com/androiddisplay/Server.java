package com.androiddisplay;

import android.content.AttributionSource;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.ContextWrapper;
import android.graphics.Rect;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.util.Range;
import android.util.Size;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Looper;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;

public final class Server {
    private static volatile boolean running = true;
    private static volatile boolean displayChanged = false;
    private static int maxVideoSize = 0;
    private static int displayWidth = 1080;
    private static int displayHeight = 2400;
    private static int videoWidth = 1080;
    private static int videoHeight = 2400;
    private static int refreshRate = 60;
    private static int nativeRefreshRate = 60;
    private static int maxFps = 0;
    private static int lockOrient = 0; // 0=auto, 1=portrait, 2=landscape
    private static int customW = 0;
    private static int customH = 0;
    private static int customDpi = 0;
    private static int targetDisplayId = 0;
    private static int targetLayerStack = 0;
    private static String videoSource = "display"; // display, new_display, sim_display, back_cam, front_cam
    private static int audioSourceCode = 8; // 8=REMOTE_SUBMIX, 1=MIC
    private static boolean optTurnScreenOff = false;
    private static boolean optStayAwake = false;
    private static boolean optShowTouches = false;
    private static String prevStayOn = null;
    private static String prevShowTouches = null;
    private static String prevOverlayDisplayDevices = null;
    private static volatile boolean settingsRestored = false;
    private static DataOutputStream ctrlOut = null;

    @SuppressWarnings("deprecation")
    public static void main(String[] args) {
        int bitrate = 8_000_000;
        boolean enableAudio = true;

        for (String arg : args) {
            if (arg.startsWith("maxSize=")) {
                maxVideoSize = Integer.parseInt(arg.substring(8));
            } else if (arg.startsWith("bitrate=")) {
                bitrate = Integer.parseInt(arg.substring(8));
            } else if (arg.startsWith("audio=")) {
                enableAudio = Boolean.parseBoolean(arg.substring(6));
            } else if (arg.startsWith("screenOff=")) {
                optTurnScreenOff = Boolean.parseBoolean(arg.substring(10));
            } else if (arg.startsWith("stayAwake=")) {
                optStayAwake = Boolean.parseBoolean(arg.substring(10));
            } else if (arg.startsWith("showTouches=")) {
                optShowTouches = Boolean.parseBoolean(arg.substring(12));
            } else if (arg.startsWith("videoSource=")) {
                videoSource = arg.substring(12);
            } else if (arg.startsWith("audioSource=")) {
                audioSourceCode = arg.substring(12).equals("mic") ? 1 : 8;
            } else if (arg.startsWith("maxFps=")) {
                maxFps = Integer.parseInt(arg.substring(7));
            } else if (arg.startsWith("lockOrient=")) {
                lockOrient = Integer.parseInt(arg.substring(11));
            } else if (arg.startsWith("customW=")) {
                customW = Integer.parseInt(arg.substring(8));
            } else if (arg.startsWith("customH=")) {
                customH = Integer.parseInt(arg.substring(8));
            } else if (arg.startsWith("customDpi=")) {
                customDpi = Integer.parseInt(arg.substring(10));
            }
        }

        if (Looper.getMainLooper() == null) {
            Looper.prepareMainLooper();
        }

        applyDeviceSettings();
        Runtime.getRuntime().addShutdownHook(new Thread(Server::restoreDeviceSettings, "adisp-shutdown"));
        queryDisplayInfo();
        nativeRefreshRate = refreshRate;
        if (maxFps > 0) {
            refreshRate = Math.min(maxFps, Math.max(60, nativeRefreshRate));
        } else if (("new_display".equals(videoSource) || "sim_display".equals(videoSource)) && refreshRate > 60) {
            refreshRate = 60;
        }
        computeVideoSize(maxVideoSize);
        if (optTurnScreenOff) {
            setPhysicalScreenPower(false);
        }

        try (
            LocalServerSocket videoServer = new LocalServerSocket("adisp_video");
            LocalServerSocket audioServer = enableAudio ? new LocalServerSocket("adisp_audio") : null;
            LocalServerSocket ctrlServer  = new LocalServerSocket("adisp_ctrl")
        ) {
            System.out.println("READY");
            System.out.flush();

            LocalSocket videoSock = videoServer.accept();
            LocalSocket audioSock = (audioServer != null) ? audioServer.accept() : null;
            LocalSocket ctrlSock  = ctrlServer.accept();

            if (ctrlSock != null) {
                Thread ctrlThread = new Thread(() -> runControlLoop(ctrlSock), "adisp-ctrl");
                ctrlThread.setDaemon(true);
                ctrlThread.start();
            }

            if (audioSock != null) {
                Thread audioThread = new Thread(() -> runAudioLoop(audioSock), "adisp-audio");
                audioThread.setDaemon(true);
                audioThread.start();
            }

            runVideoLoop(videoSock, bitrate);
        } catch (Throwable t) {
            t.printStackTrace(System.err);
        } finally {
            running = false;
            if (optTurnScreenOff) {
                setPhysicalScreenPower(true);
            }
            restoreDeviceSettings();
        }
    }

    private static Context cachedShellCtx = null;

    private static Context getShellContext() throws Exception {
        if (cachedShellCtx != null) return cachedShellCtx;
        Class<?> atClass = Class.forName("android.app.ActivityThread");
        Object at = atClass.getMethod("systemMain").invoke(null);
        Context sysCtx = (Context) atClass.getMethod("getSystemContext").invoke(at);
        Context shellCtx = sysCtx.createPackageContext("com.android.shell", Context.CONTEXT_IGNORE_SECURITY);
        try {
            java.lang.reflect.Field fOp = shellCtx.getClass().getDeclaredField("mOpPackageName");
            fOp.setAccessible(true);
            fOp.set(shellCtx, "com.android.shell");
        } catch (Throwable ignored) {}
        try {
            java.lang.reflect.Field fBase = shellCtx.getClass().getDeclaredField("mBasePackageName");
            fBase.setAccessible(true);
            fBase.set(shellCtx, "com.android.shell");
        } catch (Throwable ignored) {}
        if (Build.VERSION.SDK_INT >= 31) {
            final AttributionSource shellAttr = new AttributionSource.Builder(android.os.Process.myUid())
                .setPackageName("com.android.shell")
                .build();
            try {
                java.lang.reflect.Field fAttr = shellCtx.getClass().getDeclaredField("mAttributionSource");
                fAttr.setAccessible(true);
                fAttr.set(shellCtx, shellAttr);
            } catch (Throwable ignored) {}
            cachedShellCtx = new ContextWrapper(shellCtx) {
                @Override public Context getApplicationContext() { return this; }
                @Override public Context createAttributionContext(String attributionTag) { return this; }
                @Override public String getPackageName() { return "com.android.shell"; }
                @Override public String getOpPackageName() { return "com.android.shell"; }
                @Override public AttributionSource getAttributionSource() { return shellAttr; }
                @Override public int getDeviceId() { return 0; }
            };
            return cachedShellCtx;
        }
        cachedShellCtx = shellCtx;
        return cachedShellCtx;
    }

    private static CameraManager getCameraManager() throws Exception {
        Context ctx = getShellContext();
        try {
            java.lang.reflect.Constructor<CameraManager> ctor = CameraManager.class.getDeclaredConstructor(Context.class);
            ctor.setAccessible(true);
            return ctor.newInstance(ctx);
        } catch (Throwable t) {
            return (CameraManager) ctx.getSystemService(Context.CAMERA_SERVICE);
        }
    }

    private static String runCmdCapture(String... cmd) {
        try {
            Process p = Runtime.getRuntime().exec(cmd);
            java.io.BufferedReader br = new java.io.BufferedReader(new java.io.InputStreamReader(p.getInputStream()));
            String line = br.readLine();
            p.waitFor();
            return (line != null) ? line.trim() : null;
        } catch (Throwable t) {
            return null;
        }
    }

    private static void applyDeviceSettings() {
        if (optStayAwake) {
            prevStayOn = runCmdCapture("settings", "get", "global", "stay_on_while_plugged_in");
            runCmdCapture("settings", "put", "global", "stay_on_while_plugged_in", "7");
        }
        if (optShowTouches) {
            prevShowTouches = runCmdCapture("settings", "get", "system", "show_touches");
            runCmdCapture("settings", "put", "system", "show_touches", "1");
        }
        if ("sim_display".equals(videoSource)) {
            String simRes = (customW > 0 && customH > 0) ? (customW + "x" + customH) : "1280x720";
            int simDpi = (customDpi > 0) ? customDpi : 213;
            String simSpec = simRes + "/" + simDpi;
            prevOverlayDisplayDevices = runCmdCapture("settings", "get", "global", "overlay_display_devices");
            runCmdCapture("settings", "put", "global", "overlay_display_devices", simSpec);
            findOverlayDisplayId();
        }
    }

    private static void findOverlayDisplayId() {
        try {
            Class<?> dmgClass = Class.forName("android.hardware.display.DisplayManagerGlobal");
            Object dmg = dmgClass.getMethod("getInstance").invoke(null);
            Method getIds = dmgClass.getMethod("getDisplayIds");
            Method getInfo = dmgClass.getMethod("getDisplayInfo", int.class);
            for (int attempt = 0; attempt < 25; attempt++) {
                int[] ids = (int[]) getIds.invoke(dmg);
                if (ids != null) {
                    for (int id : ids) {
                        if (id == 0) continue;
                        Object info = getInfo.invoke(dmg, id);
                        if (info == null) continue;
                        int type = info.getClass().getField("type").getInt(info);
                        String name = (String) info.getClass().getField("name").get(info);
                        if (type == 4 || (name != null && name.startsWith("Overlay"))) {
                            targetDisplayId = id;
                            try {
                                targetLayerStack = info.getClass().getField("layerStack").getInt(info);
                            } catch (Throwable ignored) {
                                targetLayerStack = id;
                            }
                            return;
                        }
                    }
                }
                Thread.sleep(100);
            }
        } catch (Throwable t) {
            t.printStackTrace(System.err);
        }
    }

    private static synchronized void restoreDeviceSettings() {
        if (settingsRestored) return;
        settingsRestored = true;
        if (optTurnScreenOff) {
            setPhysicalScreenPower(true);
        }
        if (optStayAwake) {
            String val = (prevStayOn != null && !prevStayOn.equals("null")) ? prevStayOn : "0";
            runCmdCapture("settings", "put", "global", "stay_on_while_plugged_in", val);
        }
        if (optShowTouches) {
            String val = (prevShowTouches != null && !prevShowTouches.equals("null")) ? prevShowTouches : "0";
            runCmdCapture("settings", "put", "system", "show_touches", val);
        }
        if ("sim_display".equals(videoSource)) {
            if (prevOverlayDisplayDevices != null && !prevOverlayDisplayDevices.equals("null") && !prevOverlayDisplayDevices.isEmpty()) {
                runCmdCapture("settings", "put", "global", "overlay_display_devices", prevOverlayDisplayDevices);
            } else {
                runCmdCapture("settings", "delete", "global", "overlay_display_devices");
            }
        }
    }

    private static void setPhysicalScreenPower(boolean on) {
        int mode = on ? 2 : 0; // POWER_MODE_NORMAL=2, POWER_MODE_OFF=0
        try {
            Class<?> scClass = Class.forName("android.view.SurfaceControl");
            IBinder token = null;
            try {
                token = (IBinder) scClass.getMethod("getInternalDisplayToken").invoke(null);
            } catch (NoSuchMethodException e) {
                try {
                    long[] ids = (long[]) scClass.getMethod("getPhysicalDisplayIds").invoke(null);
                    if (ids != null && ids.length > 0) {
                        token = (IBinder) scClass.getMethod("getPhysicalDisplayToken", long.class).invoke(null, ids[0]);
                    }
                } catch (Throwable ignored) {}
            }
            if (token != null) {
                scClass.getMethod("setDisplayPowerMode", IBinder.class, int.class).invoke(null, token, mode);
                return;
            }
        } catch (Throwable ignored) {}

        // Android 14+ DisplayControl fallback (requires libandroid_servers.so)
        try {
            Class<?> dcClass = loadDisplayControlClass();
            long[] ids = (long[]) dcClass.getMethod("getPhysicalDisplayIds").invoke(null);
            if (ids != null && ids.length > 0) {
                IBinder token = (IBinder) dcClass.getMethod("getPhysicalDisplayToken", long.class).invoke(null, ids[0]);
                if (token != null) {
                    Class<?> scClass = Class.forName("android.view.SurfaceControl");
                    scClass.getMethod("setDisplayPowerMode", IBinder.class, int.class).invoke(null, token, mode);
                }
            }
        } catch (Throwable ignored) {}
    }

    @SuppressWarnings("restricted")
    private static Class<?> loadDisplayControlClass() throws Exception {
        try { System.loadLibrary("android_servers"); }
        catch (Throwable t) {
            try { System.load("/system/lib64/libandroid_servers.so"); } catch (Throwable ignored) {}
        }
        ClassLoader cl = new dalvik.system.PathClassLoader(
            "/system/framework/services.jar", ClassLoader.getSystemClassLoader());
        return Class.forName("com.android.server.display.DisplayControl", true, cl);
    }

    private static void configureDisplaySurface(Class<?> scClass, IBinder display, Surface surface) throws Exception {
        scClass.getMethod("openTransaction").invoke(null);
        try {
            scClass.getMethod("setDisplaySurface", IBinder.class, Surface.class).invoke(null, display, surface);
            scClass.getMethod("setDisplayProjection", IBinder.class, int.class, Rect.class, Rect.class)
                .invoke(null, display, 0, new Rect(0, 0, displayWidth, displayHeight), new Rect(0, 0, videoWidth, videoHeight));
            scClass.getMethod("setDisplayLayerStack", IBinder.class, int.class).invoke(null, display, targetLayerStack);
        } finally {
            scClass.getMethod("closeTransaction").invoke(null);
        }
    }

    private static void queryDisplayInfo() {
        try {
            Class<?> dmgClass = Class.forName("android.hardware.display.DisplayManagerGlobal");
            Object dmg = dmgClass.getMethod("getInstance").invoke(null);
            Object info = dmgClass.getMethod("getDisplayInfo", int.class).invoke(dmg, targetDisplayId);
            if (info != null) {
                displayWidth = info.getClass().getField("logicalWidth").getInt(info);
                displayHeight = info.getClass().getField("logicalHeight").getInt(info);
                try {
                    targetLayerStack = info.getClass().getField("layerStack").getInt(info);
                } catch (Throwable ignored) {
                    targetLayerStack = targetDisplayId;
                }
                try {
                    Object mode = info.getClass().getMethod("getMode").invoke(info);
                    if (mode != null) {
                        float rr = (Float) mode.getClass().getMethod("getRefreshRate").invoke(mode);
                        if (rr >= 59.0f) {
                            refreshRate = Math.min(120, Math.round(rr));
                        }
                    }
                } catch (Throwable ignored) {}
            }
        } catch (Throwable t) {
            t.printStackTrace(System.err);
        }
    }

    private static boolean isMainDisplaySource() {
        return "display".equals(videoSource) || "sim_display".equals(videoSource);
    }

    private static void computeVideoSize(int maxSize) {
        if ("new_display".equals(videoSource)) {
            int w = (customW > 0) ? customW : 1920;
            int h = (customH > 0) ? customH : 1080;
            if (lockOrient == 1 && w > h) { int t = w; w = h; h = t; }
            else if (lockOrient == 2 && h > w) { int t = w; w = h; h = t; }
            videoWidth = Math.max(128, w & ~15);
            videoHeight = Math.max(128, h & ~15);
            return;
        }
        int w = displayWidth;
        int h = displayHeight;
        if (!isMainDisplaySource()) {
            w = 1280;
            h = 720;
        } else if (lockOrient == 1 && w > h) {
            int t = w; w = h; h = t;
        } else if (lockOrient == 2 && h > w) {
            int t = w; w = h; h = t;
        }
        if (maxSize > 0) {
            int minDim = Math.min(w, h);
            if (minDim > maxSize) {
                w = (w * maxSize) / minDim;
                h = (h * maxSize) / minDim;
            }
        }
        videoWidth = Math.max(128, w & ~1);
        videoHeight = Math.max(128, h & ~1);
    }

    private static VirtualDisplay virtualDisplay = null;

    private static IBinder createNewDesktopDisplay(Surface surface) throws Exception {
        Context shellCtx = getShellContext();
        DisplayManager dm = (DisplayManager) shellCtx.getSystemService(Context.DISPLAY_SERVICE);
        int dpi = (customDpi > 0) ? customDpi : 240;
        // PUBLIC(1) | OWN_CONTENT_ONLY(8) | SUPPORTS_TOUCH(64) | ROTATES_WITH_CONTENT(128) | DESTROY_CONTENT_ON_REMOVAL(256) | SHOULD_SHOW_SYSTEM_DECORATIONS(512) | TRUSTED(1024)
        int flags = DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC
            | DisplayManager.VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY
            | (1 << 6)
            | (1 << 7)
            | (1 << 8)
            | (1 << 9)
            | (1 << 10);
        try {
            virtualDisplay = dm.createVirtualDisplay("android-display-desktop", videoWidth, videoHeight, dpi, surface, flags);
        } catch (Throwable t) {
            int fallbackFlags = DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC
                | DisplayManager.VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY
                | (1 << 9);
            try {
                virtualDisplay = dm.createVirtualDisplay("android-display-desktop", videoWidth, videoHeight, dpi, surface, fallbackFlags);
            } catch (Throwable t2) {
                virtualDisplay = dm.createVirtualDisplay("android-display-desktop", videoWidth, videoHeight, dpi, surface,
                    DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC | DisplayManager.VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY);
            }
        }
        if (virtualDisplay == null) {
            throw new IllegalStateException("createVirtualDisplay returned null");
        }
        targetDisplayId = virtualDisplay.getDisplay().getDisplayId();
        return null;
    }

    private static IBinder createDisplaySurface(Surface surface) throws Exception {
        if ("new_display".equals(videoSource)) {
            return createNewDesktopDisplay(surface);
        }
        if (!"sim_display".equals(videoSource)) {
            targetDisplayId = 0;
            targetLayerStack = 0;
        }
        Class<?> scClass = Class.forName("android.view.SurfaceControl");
        IBinder display = null;
        try {
            Method createDisplay = scClass.getMethod("createDisplay", String.class, boolean.class);
            display = (IBinder) createDisplay.invoke(null, "android-display", false);
        } catch (NoSuchMethodException ignored) {}

        if (display != null) {
            configureDisplaySurface(scClass, display, surface);
            return display;
        }

        // Android 14+: try DisplayControl (services.jar + libandroid_servers)
        try {
            Class<?> dcClass = loadDisplayControlClass();
            Method createDisplay = dcClass.getMethod("createDisplay", String.class, boolean.class);
            display = (IBinder) createDisplay.invoke(null, "android-display", false);
            if (display != null) {
                configureDisplaySurface(scClass, display, surface);
                return display;
            }
        } catch (Throwable t) {
            t.printStackTrace(System.err);
        }

        // Android 14/15/16: DisplayManager.createVirtualDisplay with com.android.shell Context (UID 2000)
        Class<?> atClass = Class.forName("android.app.ActivityThread");
        Object at = atClass.getMethod("systemMain").invoke(null);
        Context sysCtx = (Context) atClass.getMethod("getSystemContext").invoke(at);
        Context shellCtx = sysCtx.createPackageContext("com.android.shell", Context.CONTEXT_IGNORE_SECURITY);
        DisplayManager dm = (DisplayManager) shellCtx.getSystemService(Context.DISPLAY_SERVICE);
        int flags = DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC | DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR;
        if (targetDisplayId > 0) {
            try {
                Class<?> cfgClass = Class.forName("android.hardware.display.VirtualDisplayConfig");
                Class<?> bldClass = Class.forName("android.hardware.display.VirtualDisplayConfig$Builder");
                Object builder = bldClass.getConstructor(String.class, int.class, int.class, int.class)
                    .newInstance("android-display", videoWidth, videoHeight, 320);
                bldClass.getMethod("setFlags", int.class).invoke(builder, flags);
                bldClass.getMethod("setSurface", Surface.class).invoke(builder, surface);
                bldClass.getMethod("setDisplayIdToMirror", int.class).invoke(builder, targetDisplayId);
                Object cfg = bldClass.getMethod("build").invoke(builder);
                virtualDisplay = (VirtualDisplay) dm.getClass().getMethod("createVirtualDisplay", cfgClass).invoke(dm, cfg);
            } catch (Throwable ignored) {}
        }
        if (virtualDisplay == null) {
            virtualDisplay = dm.createVirtualDisplay("android-display", videoWidth, videoHeight, 320, surface, flags);
        }
        if (virtualDisplay == null) {
            throw new IllegalStateException("createVirtualDisplay returned null");
        }
        return null;
    }

    private static void destroyDisplaySurface(IBinder display) {
        if (virtualDisplay != null) {
            try { virtualDisplay.release(); } catch (Throwable ignored) {}
            virtualDisplay = null;
        }
        if (display == null) return;
        try {
            Class<?> scClass = Class.forName("android.view.SurfaceControl");
            scClass.getMethod("destroyDisplay", IBinder.class).invoke(null, display);
        } catch (Throwable ignored) {}
    }

    private static CameraDevice activeCamera = null;
    private static CameraCaptureSession activeSession = null;
    private static Handler activeCamHandler = null;
    private static Rect activeSensorRect = null;
    private static float maxCamZoom = 4.0f;
    private static float currentCamZoom = 1.0f;

    private static String chosenCameraId = null;
    private static Range<Integer> chosenCamFps = null;

    private static void prepareCameraConfig() {
        try {
            CameraManager cm = getCameraManager();
            int wantFacing = "front_cam".equals(videoSource)
                ? CameraCharacteristics.LENS_FACING_FRONT
                : CameraCharacteristics.LENS_FACING_BACK;

            chosenCameraId = null;
            for (String id : cm.getCameraIdList()) {
                CameraCharacteristics cc = cm.getCameraCharacteristics(id);
                Integer facing = cc.get(CameraCharacteristics.LENS_FACING);
                if (facing != null && facing == wantFacing) {
                    chosenCameraId = id;
                    break;
                }
            }
            if (chosenCameraId == null && cm.getCameraIdList().length > 0) {
                chosenCameraId = cm.getCameraIdList()[0];
            }
            if (chosenCameraId != null) {
                CameraCharacteristics cc = cm.getCameraCharacteristics(chosenCameraId);
                StreamConfigurationMap map = cc.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
                if (map != null) {
                    Size[] sizes = map.getOutputSizes(MediaCodec.class);
                    if (sizes != null && sizes.length > 0) {
                        MediaCodecInfo.VideoCapabilities vc = null;
                        try {
                            MediaCodec probe = MediaCodec.createEncoderByType("video/avc");
                            vc = probe.getCodecInfo().getCapabilitiesForType("video/avc").getVideoCapabilities();
                            probe.release();
                        } catch (Throwable ignored) {}

                        Size best = null;
                        long bestArea = -1;
                        int wantW = (maxVideoSize > 0) ? (maxVideoSize * 16 / 9) : 0;
                        int wantH = maxVideoSize;
                        long maxFrameDurNs = (maxFps >= 60) ? 17_500_000L : 34_000_000L;
                        for (Size s : sizes) {
                            int sw = s.getWidth();
                            int sh = s.getHeight();
                            int minDim = Math.min(sw, sh);
                            int maxDim = Math.max(sw, sh);
                            if (maxVideoSize > 0 && minDim > maxVideoSize) {
                                continue;
                            }
                            // Cap Native camera stream at 2560px (e.g. 1440p/1080p) to avoid 4K/12MP ISP lag
                            if (maxVideoSize == 0 && maxDim > 2560) {
                                continue;
                            }
                            if (vc != null && !vc.isSizeSupported(sw, sh)) {
                                continue;
                            }
                            try {
                                long minDur = map.getOutputMinFrameDuration(MediaCodec.class, s);
                                if (minDur > 0 && minDur > maxFrameDurNs) {
                                    continue;
                                }
                            } catch (Throwable ignored) {}
                            if (maxVideoSize > 0 && minDim == wantH && maxDim == wantW) {
                                best = s;
                                break;
                            }
                            long area = (long) sw * (long) sh;
                            if (area > bestArea) {
                                bestArea = area;
                                best = s;
                            }
                        }
                        if (best == null) {
                            best = sizes[0];
                            for (Size s : sizes) {
                                long area = (long) s.getWidth() * (long) s.getHeight();
                                if (bestArea < 0 || area < bestArea) {
                                    bestArea = area;
                                    best = s;
                                }
                            }
                        }
                        videoWidth = best.getWidth();
                        videoHeight = best.getHeight();
                    }
                }
                @SuppressWarnings("unchecked")
                Range<Integer>[] ranges = cc.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
                chosenCamFps = null;
                if (ranges != null) {
                    int wantCap = (maxFps > 0) ? maxFps : 60;
                    for (Range<Integer> r : ranges) {
                        if (r.getUpper() <= wantCap && r.getUpper() >= 30) {
                            if (chosenCamFps == null ||
                                r.getUpper() > chosenCamFps.getUpper() ||
                                (r.getUpper().equals(chosenCamFps.getUpper()) && r.getLower() > chosenCamFps.getLower())) {
                                chosenCamFps = r;
                            }
                        }
                    }
                }
                refreshRate = (chosenCamFps != null) ? chosenCamFps.getUpper() : 30;
                activeSensorRect = cc.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE);
                Float mz = cc.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM);
                maxCamZoom = (mz != null && mz >= 1.0f) ? Math.min(8.0f, mz) : 1.0f;
                currentCamZoom = 1.0f;
            }
        } catch (Throwable ignored) {}
    }

    private static void applyZoomAndRepeat(boolean triggerAf) {
        if (activeCamera == null || activeSession == null || activeCamHandler == null) return;
        try {
            CaptureRequest.Builder req = activeCamera.createCaptureRequest(CameraDevice.TEMPLATE_RECORD);
            req.set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO);
            if (chosenCamFps != null) {
                req.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, chosenCamFps);
            }
            float z = Math.max(1.0f, Math.min(maxCamZoom, currentCamZoom));
            if (Build.VERSION.SDK_INT >= 30) {
                req.set(CaptureRequest.CONTROL_ZOOM_RATIO, z);
            } else if (activeSensorRect != null && z > 1.0f) {
                int cw = (int) (activeSensorRect.width() / z);
                int ch = (int) (activeSensorRect.height() / z);
                int cx = activeSensorRect.centerX();
                int cy = activeSensorRect.centerY();
                req.set(CaptureRequest.SCALER_CROP_REGION, new Rect(cx - cw / 2, cy - ch / 2, cx + cw / 2, cy + ch / 2));
            }
            if (activeSurface != null) {
                req.addTarget(activeSurface);
            }
            if (triggerAf) {
                req.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_AUTO);
                req.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_START);
                activeSession.capture(req.build(), null, activeCamHandler);
                req.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_IDLE);
            }
            req.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO);
            activeSession.setRepeatingRequest(req.build(), null, activeCamHandler);
        } catch (Throwable ignored) {}
    }

    private static Surface activeSurface = null;

    private static void openCameraToSurface(Surface surface, Handler handler) throws Exception {
        activeSurface = surface;
        activeCamHandler = handler;
        CameraManager cm = getCameraManager();
        if (chosenCameraId == null) {
            prepareCameraConfig();
        }
        if (chosenCameraId == null) {
            throw new IllegalStateException("No camera found");
        }

        final Object lock = new Object();
        final boolean[] done = { false };
        final String[] errMsg = { null };
        cm.openCamera(chosenCameraId, new CameraDevice.StateCallback() {
            @SuppressWarnings("deprecation")
            @Override
            public void onOpened(CameraDevice camera) {
                activeCamera = camera;
                try {
                    CameraCaptureSession.StateCallback cb = new CameraCaptureSession.StateCallback() {
                            @Override
                            public void onConfigured(CameraCaptureSession session) {
                                activeSession = session;
                                try {
                                    CaptureRequest.Builder req = camera.createCaptureRequest(CameraDevice.TEMPLATE_RECORD);
                                    req.set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO);
                                    req.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO);
                                    try {
                                        req.set(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE, CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_OFF);
                                        req.set(CaptureRequest.NOISE_REDUCTION_MODE, CaptureRequest.NOISE_REDUCTION_MODE_FAST);
                                        req.set(CaptureRequest.EDGE_MODE, CaptureRequest.EDGE_MODE_FAST);
                                    } catch (Throwable ignored) {}
                                    if (chosenCamFps != null) {
                                        req.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, chosenCamFps);
                                    }
                                    req.addTarget(surface);
                                    session.setRepeatingRequest(req.build(), null, handler);
                                } catch (Throwable t) {
                                    errMsg[0] = "Camera request failed: " + t.getMessage();
                                }
                                synchronized (lock) { done[0] = true; lock.notifyAll(); }
                            }
                            @Override
                            public void onConfigureFailed(CameraCaptureSession session) {
                                errMsg[0] = "Camera session configure failed (" + videoWidth + "x" + videoHeight + ")";
                                synchronized (lock) { done[0] = true; lock.notifyAll(); }
                            }
                        };
                    if (Build.VERSION.SDK_INT >= 28) {
                        android.hardware.camera2.params.OutputConfiguration outCfg =
                            new android.hardware.camera2.params.OutputConfiguration(surface);
                        android.hardware.camera2.params.SessionConfiguration sessCfg =
                            new android.hardware.camera2.params.SessionConfiguration(
                                android.hardware.camera2.params.SessionConfiguration.SESSION_REGULAR,
                                java.util.Collections.singletonList(outCfg),
                                cmd -> handler.post(cmd),
                                cb);
                        camera.createCaptureSession(sessCfg);
                    } else {
                        camera.createCaptureSession(java.util.Collections.singletonList(surface), cb, handler);
                    }
                } catch (Throwable t) {
                    errMsg[0] = "Camera session error: " + t.getMessage();
                    synchronized (lock) { done[0] = true; lock.notifyAll(); }
                }
            }
            @Override public void onDisconnected(CameraDevice camera) { camera.close(); }
            @Override public void onError(CameraDevice camera, int error) {
                camera.close();
                errMsg[0] = "Camera open error: " + error;
                synchronized (lock) { done[0] = true; lock.notifyAll(); }
            }
        }, handler);

        synchronized (lock) {
            if (!done[0]) lock.wait(5000);
        }
        if (!done[0]) {
            throw new IllegalStateException("Timed out opening camera");
        }
        if (errMsg[0] != null) {
            throw new IllegalStateException(errMsg[0]);
        }
    }

    private static void closeCamera() {
        if (activeSession != null) {
            try { activeSession.close(); } catch (Throwable ignored) {}
            activeSession = null;
        }
        if (activeCamera != null) {
            try { activeCamera.close(); } catch (Throwable ignored) {}
            activeCamera = null;
        }
    }

    private static void runVideoLoop(LocalSocket sock, int bitrate) {
        HandlerThread dispThread = new HandlerThread("adisp-disp");
        dispThread.start();
        DisplayManager dmListener = null;
        DisplayManager.DisplayListener listener = null;

        try {
            if (isMainDisplaySource()) {
                Class<?> atClass = Class.forName("android.app.ActivityThread");
                Object at = atClass.getMethod("systemMain").invoke(null);
                Context sysCtx = (Context) atClass.getMethod("getSystemContext").invoke(at);
                dmListener = (DisplayManager) sysCtx.getSystemService(Context.DISPLAY_SERVICE);
                listener = new DisplayManager.DisplayListener() {
                    @Override public void onDisplayAdded(int displayId) {}
                    @Override public void onDisplayRemoved(int displayId) {}
                    @Override public void onDisplayChanged(int displayId) {
                        if (displayId == targetDisplayId) {
                            int prevW = displayWidth;
                            int prevH = displayHeight;
                            queryDisplayInfo();
                            if (displayWidth != prevW || displayHeight != prevH) {
                                computeVideoSize(maxVideoSize);
                                displayChanged = true;
                            }
                        }
                    }
                };
                dmListener.registerDisplayListener(listener, new Handler(dispThread.getLooper()));
            }
        } catch (Throwable ignored) {}

        try {
            try { sock.setSendBufferSize(256 * 1024); } catch (Throwable ignored) {}
            java.io.OutputStream rawOut = sock.getOutputStream();
            DataOutputStream out = new DataOutputStream(rawOut);
            boolean sentHeader = false;
            byte[] pktBuf = new byte[256 * 1024];

            while (running) {
                displayChanged = false;
                MediaCodec codec = null;
                IBinder display = null;
                Surface surface = null;

                try {
                    if ("back_cam".equals(videoSource) || "front_cam".equals(videoSource)) {
                        prepareCameraConfig();
                    }
                    MediaFormat fmt = MediaFormat.createVideoFormat("video/avc", videoWidth, videoHeight);
                    fmt.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
                    fmt.setInteger(MediaFormat.KEY_BIT_RATE, bitrate);
                    fmt.setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR);
                    fmt.setInteger(MediaFormat.KEY_FRAME_RATE, refreshRate);
                    fmt.setFloat(MediaFormat.KEY_OPERATING_RATE, (float) Math.max(120, refreshRate));
                    if (Build.VERSION.SDK_INT >= 29 && refreshRate < nativeRefreshRate) {
                        fmt.setFloat("max-fps-to-encoder", (float) refreshRate);
                    }
                    fmt.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 10);
                    fmt.setLong(MediaFormat.KEY_REPEAT_PREVIOUS_FRAME_AFTER, 100_000L);
                    fmt.setInteger(MediaFormat.KEY_PRIORITY, 0);
                    if (Build.VERSION.SDK_INT >= 26) {
                        fmt.setInteger("latency", 0);
                    }
                    if (Build.VERSION.SDK_INT >= 29) {
                        fmt.setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0);
                        fmt.setInteger("max-bframes", 0);
                    }
                    if (Build.VERSION.SDK_INT >= 30) {
                        fmt.setInteger(MediaFormat.KEY_LOW_LATENCY, 1);
                        fmt.setInteger("low-latency", 1);
                    }

                    codec = MediaCodec.createEncoderByType("video/avc");
                    codec.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
                    surface = codec.createInputSurface();
                    if ("back_cam".equals(videoSource) || "front_cam".equals(videoSource)) {
                        openCameraToSurface(surface, new Handler(dispThread.getLooper()));
                    } else {
                        display = createDisplaySurface(surface);
                        if (optTurnScreenOff) {
                            setPhysicalScreenPower(false);
                        }
                    }
                    codec.start();
                    if (Build.VERSION.SDK_INT >= 30) {
                        try {
                            Bundle params = new Bundle();
                            params.putInt(MediaCodec.PARAMETER_KEY_LOW_LATENCY, 1);
                            codec.setParameters(params);
                        } catch (Throwable ignored) {}
                    }

                    if (!sentHeader) {
                        out.writeInt(videoWidth);
                        out.writeInt(videoHeight);
                        out.flush();
                        sentHeader = true;
                    }

                    MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
                    while (running && !displayChanged) {
                        int outIdx = codec.dequeueOutputBuffer(info, 10_000);
                        if (outIdx >= 0) {
                            ByteBuffer buf = codec.getOutputBuffer(outIdx);
                            if (buf != null && info.size > 0) {
                                int sz = info.size;
                                if (pktBuf.length < 4 + sz) {
                                    pktBuf = new byte[4 + sz];
                                }
                                pktBuf[0] = (byte) ((sz >>> 24) & 0xFF);
                                pktBuf[1] = (byte) ((sz >>> 16) & 0xFF);
                                pktBuf[2] = (byte) ((sz >>> 8) & 0xFF);
                                pktBuf[3] = (byte) (sz & 0xFF);
                                buf.position(info.offset);
                                buf.limit(info.offset + sz);
                                buf.get(pktBuf, 4, sz);
                                rawOut.write(pktBuf, 0, 4 + sz);
                            }
                            codec.releaseOutputBuffer(outIdx, false);
                            if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                                break;
                            }
                        }
                    }
                } finally {
                    closeCamera();
                    destroyDisplaySurface(display);
                    if (codec != null) {
                        try { codec.stop(); } catch (Throwable ignored) {}
                        try { codec.release(); } catch (Throwable ignored) {}
                    }
                    if (surface != null) {
                        try { surface.release(); } catch (Throwable ignored) {}
                    }
                }
            }
        } catch (Throwable t) {
            t.printStackTrace(System.err);
        } finally {
            running = false;
            if (dmListener != null && listener != null) {
                try { dmListener.unregisterDisplayListener(listener); } catch (Throwable ignored) {}
            }
            dispThread.quitSafely();
            try { sock.close(); } catch (IOException ignored) {}
        }
    }

    private static AudioAttributes buildCaptureAttributes(int source) throws Exception {
        AudioAttributes.Builder ab = new AudioAttributes.Builder();
        Method setPreset = AudioAttributes.Builder.class.getMethod("setInternalCapturePreset", int.class);
        setPreset.invoke(ab, source);
        return ab.build();
    }

    private static void trySetBuilderContext(AudioRecord.Builder b) {
        if (Build.VERSION.SDK_INT >= 31) {
            try {
                Method setCtx = AudioRecord.Builder.class.getMethod("setContext", Context.class);
                setCtx.invoke(b, getShellContext());
            } catch (Throwable ignored) {}
        }
    }

    private static void runAudioLoop(LocalSocket sock) {
        if (Looper.myLooper() == null) {
            Looper.prepare();
        }
        AudioRecord recorder = null;
        try {
            int sampleRate = 48000;
            int minBuf = AudioRecord.getMinBufferSize(
                sampleRate, AudioFormat.CHANNEL_IN_STEREO, AudioFormat.ENCODING_PCM_16BIT);
            int bufSize = Math.max(minBuf, 4096);

            AudioFormat audioFmt = new AudioFormat.Builder()
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setSampleRate(sampleRate)
                .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
                .build();

            // Attempt 1: AudioRecord.Builder with setAudioSource + shell Context
            try {
                AudioRecord.Builder b1 = new AudioRecord.Builder()
                    .setAudioFormat(audioFmt)
                    .setBufferSizeInBytes(bufSize);
                trySetBuilderContext(b1);
                Method setSource = AudioRecord.Builder.class.getMethod("setAudioSource", int.class);
                setSource.invoke(b1, audioSourceCode);
                recorder = b1.build();
            } catch (Throwable ignored) {}

            // Attempt 2: Direct AudioRecord(AudioAttributes, AudioFormat, bufSize, 0)
            if (recorder == null || recorder.getState() != AudioRecord.STATE_INITIALIZED) {
                if (recorder != null) { try { recorder.release(); } catch (Throwable ignored) {} recorder = null; }
                try {
                    java.lang.reflect.Constructor<AudioRecord> ctor = AudioRecord.class.getDeclaredConstructor(
                        AudioAttributes.class, AudioFormat.class, int.class, int.class);
                    ctor.setAccessible(true);
                    recorder = ctor.newInstance(buildCaptureAttributes(audioSourceCode), audioFmt, bufSize, 0);
                } catch (Throwable ignored) {}
            }

            // Attempt 3: AudioRecord.Builder with setAudioAttributes + shell Context
            if (recorder == null || recorder.getState() != AudioRecord.STATE_INITIALIZED) {
                if (recorder != null) { try { recorder.release(); } catch (Throwable ignored) {} recorder = null; }
                try {
                    AudioRecord.Builder b3 = new AudioRecord.Builder()
                        .setAudioFormat(audioFmt)
                        .setBufferSizeInBytes(bufSize);
                    Method setAttrs = AudioRecord.Builder.class.getMethod("setAudioAttributes", AudioAttributes.class);
                    setAttrs.invoke(b3, buildCaptureAttributes(audioSourceCode));
                    trySetBuilderContext(b3);
                    recorder = b3.build();
                } catch (Throwable ignored) {}
            }

            if (recorder == null || recorder.getState() != AudioRecord.STATE_INITIALIZED) {
                return;
            }

            DataOutputStream out = new DataOutputStream(sock.getOutputStream());
            if (Build.VERSION.SDK_INT == 30) {
                try {
                    Process p = Runtime.getRuntime().exec(new String[]{
                        "am", "start", "-n", "com.android.shell/.HeapDumpActivity"
                    });
                    p.waitFor();
                    Thread.sleep(120);
                } catch (Throwable ignored) {}
            }
            recorder.startRecording();
            if (Build.VERSION.SDK_INT == 30) {
                try {
                    Runtime.getRuntime().exec(new String[]{ "am", "force-stop", "com.android.shell" });
                } catch (Throwable ignored) {}
            }
            byte[] pcm = new byte[3840]; // 20ms of 48kHz 16-bit stereo
            while (running) {
                int read = recorder.read(pcm, 0, pcm.length);
                if (read > 0) {
                    out.writeInt(read);
                    out.write(pcm, 0, read);
                    out.flush();
                } else if (read < 0) {
                    break;
                }
            }
        } catch (Throwable ignored) {
            // Audio capture not available on this device/OS
        } finally {
            if (recorder != null) {
                try { recorder.stop(); } catch (Throwable ignored) {}
                try { recorder.release(); } catch (Throwable ignored) {}
            }
            try { sock.close(); } catch (IOException ignored) {}
        }
    }

    private static void injectWithDisplay(Method injectMethod, Object im, Method setDisplayId, InputEvent ev) throws Exception {
        if (targetDisplayId > 0 && setDisplayId != null) {
            try { setDisplayId.invoke(ev, targetDisplayId); } catch (Throwable ignored) {}
        }
        injectMethod.invoke(im, ev, 0);
    }

    private static void runControlLoop(LocalSocket sock) {
        try {
            Object im = null;
            Method injectMethod = null;
            try {
                // Android 14 - 17
                Class<?> imgClass = Class.forName("android.hardware.input.InputManagerGlobal");
                im = imgClass.getMethod("getInstance").invoke(null);
                injectMethod = imgClass.getMethod("injectInputEvent", InputEvent.class, int.class);
            } catch (Throwable t) {
                // Android 7 - 13
                Class<?> imClass = Class.forName("android.hardware.input.InputManager");
                im = imClass.getMethod("getInstance").invoke(null);
                injectMethod = imClass.getMethod("injectInputEvent", InputEvent.class, int.class);
            }

            Method setDisplayIdMethod = null;
            try {
                setDisplayIdMethod = InputEvent.class.getMethod("setDisplayId", int.class);
            } catch (Throwable ignored) {}
            final Method fSetDisplayId = setDisplayIdMethod;
            final Method fInject = injectMethod;
            final Object fIm = im;

            DataInputStream in = new DataInputStream(sock.getInputStream());
            ctrlOut = new DataOutputStream(sock.getOutputStream());
            long downTime = 0;

            try {
                Context clipCtx = getShellContext();
                ClipboardManager cm = (ClipboardManager) clipCtx.getSystemService(Context.CLIPBOARD_SERVICE);
                if (cm != null) {
                    cm.addPrimaryClipChangedListener(() -> {
                        try {
                            ClipData cd = cm.getPrimaryClip();
                            if (cd != null && cd.getItemCount() > 0) {
                                CharSequence cs = cd.getItemAt(0).getText();
                                if (cs != null && !"AndroidDisplay".equals(cd.getDescription().getLabel())) {
                                    byte[] utf8 = cs.toString().getBytes(StandardCharsets.UTF_8);
                                    if (utf8.length > 0 && utf8.length <= 60000 && ctrlOut != null) {
                                        synchronized (ctrlOut) {
                                            ctrlOut.writeByte(5);
                                            ctrlOut.writeShort(utf8.length);
                                            ctrlOut.write(utf8);
                                            ctrlOut.flush();
                                        }
                                    }
                                }
                            }
                        } catch (Throwable ignored) {}
                    });
                }
            } catch (Throwable ignored) {}

            while (running) {
                int type = in.readUnsignedByte();
                boolean isCameraMode = "back_cam".equals(videoSource) || "front_cam".equals(videoSource);
                if (type == 1) {
                    int action = in.readUnsignedByte();
                    int x = in.readUnsignedShort();
                    int y = in.readUnsignedShort();
                    int w = Math.max(1, in.readUnsignedShort());
                    int h = Math.max(1, in.readUnsignedShort());
                    if (isCameraMode) continue;

                    int targetW = (targetDisplayId > 0) ? videoWidth : displayWidth;
                    int targetH = (targetDisplayId > 0) ? videoHeight : displayHeight;
                    float fx = (x * (float) targetW) / w;
                    float fy = (y * (float) targetH) / h;
                    long now = SystemClock.uptimeMillis();
                    MotionEvent.PointerProperties[] props = { new MotionEvent.PointerProperties() };
                    props[0].id = 0;
                    props[0].toolType = MotionEvent.TOOL_TYPE_MOUSE;
                    MotionEvent.PointerCoords[] coords = { new MotionEvent.PointerCoords() };
                    coords[0].x = fx;
                    coords[0].y = fy;
                    coords[0].pressure = (action == 3 || action == 1) ? 0.0f : 1.0f;
                    coords[0].size = 1.0f;

                    int motionAction;
                    int buttonState = 0;
                    long evDownTime = downTime;
                    if (action == 3) {
                        motionAction = MotionEvent.ACTION_HOVER_MOVE;
                        evDownTime = now;
                    } else if (action == 0) {
                        downTime = now;
                        evDownTime = now;
                        motionAction = MotionEvent.ACTION_DOWN;
                        buttonState = MotionEvent.BUTTON_PRIMARY;
                    } else if (action == 1) {
                        motionAction = MotionEvent.ACTION_UP;
                    } else {
                        motionAction = MotionEvent.ACTION_MOVE;
                        buttonState = MotionEvent.BUTTON_PRIMARY;
                    }

                    MotionEvent ev = MotionEvent.obtain(
                        evDownTime, now, motionAction, 1, props, coords,
                        0, buttonState, 1.0f, 1.0f, 0, 0, InputDevice.SOURCE_MOUSE, 0);
                    injectWithDisplay(fInject, fIm, fSetDisplayId, ev);
                    ev.recycle();
                } else if (type == 2) {
                    int keyCode = in.readInt();
                    if (isCameraMode) continue;
                    long now = SystemClock.uptimeMillis();
                    KeyEvent down = new KeyEvent(now, now, KeyEvent.ACTION_DOWN, keyCode, 0);
                    KeyEvent up = new KeyEvent(now, now, KeyEvent.ACTION_UP, keyCode, 0);
                    injectWithDisplay(fInject, fIm, fSetDisplayId, down);
                    injectWithDisplay(fInject, fIm, fSetDisplayId, up);
                } else if (type == 3) {
                    int x = in.readUnsignedShort();
                    int y = in.readUnsignedShort();
                    int w = Math.max(1, in.readUnsignedShort());
                    int h = Math.max(1, in.readUnsignedShort());
                    int vScroll = in.readByte();
                    if (isCameraMode) continue;

                    int targetW = (targetDisplayId > 0) ? videoWidth : displayWidth;
                    int targetH = (targetDisplayId > 0) ? videoHeight : displayHeight;
                    float fx = (x * (float) targetW) / w;
                    float fy = (y * (float) targetH) / h;
                    long now = SystemClock.uptimeMillis();

                    MotionEvent.PointerProperties[] props = { new MotionEvent.PointerProperties() };
                    props[0].id = 0;
                    props[0].toolType = MotionEvent.TOOL_TYPE_MOUSE;
                    MotionEvent.PointerCoords[] coords = { new MotionEvent.PointerCoords() };
                    coords[0].x = fx;
                    coords[0].y = fy;
                    coords[0].setAxisValue(MotionEvent.AXIS_VSCROLL, (float) vScroll);

                    MotionEvent ev = MotionEvent.obtain(
                        now, now, MotionEvent.ACTION_SCROLL, 1, props, coords,
                        0, 0, 1.0f, 1.0f, 0, 0, InputDevice.SOURCE_MOUSE, 0);
                    injectWithDisplay(fInject, fIm, fSetDisplayId, ev);
                    ev.recycle();
                } else if (type == 4) {
                    int len = in.readUnsignedShort();
                    if (len > 0 && len <= 1024) {
                        byte[] bytes = new byte[len];
                        in.readFully(bytes);
                        if (isCameraMode) continue;
                        String text = new String(bytes, StandardCharsets.UTF_8);
                        KeyCharacterMap kcm = KeyCharacterMap.load(KeyCharacterMap.VIRTUAL_KEYBOARD);
                        KeyEvent[] events = kcm.getEvents(text.toCharArray());
                        if (events != null) {
                            for (KeyEvent ke : events) {
                                injectWithDisplay(fInject, fIm, fSetDisplayId, ke);
                            }
                        }
                    }
                } else if (type == 5) {
                    // SET_CLIPBOARD: [paste:1][len:2][utf8]
                    boolean paste = (in.readUnsignedByte() != 0);
                    int len = in.readUnsignedShort();
                    if (len >= 0 && len <= 65535) {
                        byte[] bytes = new byte[len];
                        if (len > 0) in.readFully(bytes);
                        String clipText = new String(bytes, StandardCharsets.UTF_8);
                        try {
                            Context ctx = getShellContext();
                            ClipboardManager cm = (ClipboardManager) ctx.getSystemService(Context.CLIPBOARD_SERVICE);
                            if (cm != null) {
                                cm.setPrimaryClip(ClipData.newPlainText("AndroidDisplay", clipText));
                            }
                        } catch (Throwable ignored) {}
                        if (paste) {
                            long now = SystemClock.uptimeMillis();
                            KeyEvent down = new KeyEvent(now, now, KeyEvent.ACTION_DOWN, 279 /* KEYCODE_PASTE */, 0);
                            KeyEvent up = new KeyEvent(now, now, KeyEvent.ACTION_UP, 279, 0);
                            injectWithDisplay(fInject, fIm, fSetDisplayId, down);
                            injectWithDisplay(fInject, fIm, fSetDisplayId, up);
                        }
                    }
                } else if (type == 6) {
                    // SET_SCREEN_POWER: [on:1]
                    boolean on = (in.readUnsignedByte() != 0);
                    setPhysicalScreenPower(on);
                } else if (type == 7) {
                    // EXPAND_NOTIFICATIONS: [expand:1]
                    boolean expand = (in.readUnsignedByte() != 0);
                    try {
                        Runtime.getRuntime().exec(new String[]{
                            "cmd", "statusbar", expand ? "expand-notifications" : "collapse"
                        });
                    } catch (Throwable ignored) {}
                } else if (type == 8) {
                    // ROTATE_DEVICE (toggle Portrait <-> Landscape)
                    try {
                        Class<?> smClass = Class.forName("android.os.ServiceManager");
                        IBinder wmb = (IBinder) smClass.getMethod("getService", String.class).invoke(null, "window");
                        Class<?> iwmStub = Class.forName("android.view.IWindowManager$Stub");
                        Object iwm = iwmStub.getMethod("asInterface", IBinder.class).invoke(null, wmb);

                        int cur = 0;
                        try {
                            cur = (Integer) iwm.getClass().getMethod("getDefaultDisplayRotation").invoke(iwm);
                        } catch (Throwable t) {
                            Context ctx = getShellContext();
                            cur = android.provider.Settings.System.getInt(ctx.getContentResolver(), "user_rotation", 0);
                        }
                        int nextRot = (cur == 0) ? 1 : 0;

                        boolean rotated = false;
                        if (targetDisplayId != 0) {
                            try {
                                iwm.getClass().getMethod("freezeDisplayRotation", int.class, int.class, String.class)
                                    .invoke(iwm, targetDisplayId, nextRot, "adisp");
                                rotated = true;
                            } catch (Throwable ignored) {}
                        }
                        if (!rotated) {
                            try {
                                iwm.getClass().getMethod("freezeRotation", int.class).invoke(iwm, nextRot);
                                rotated = true;
                            } catch (Throwable ignored) {}
                        }
                        if (!rotated) {
                            Runtime.getRuntime().exec(new String[]{
                                "wm", "user-rotation", "lock", String.valueOf(nextRot)
                            });
                        }
                    } catch (Throwable ignored) {}
                } else if (type == 9) {
                    // CAMERA_ZOOM: [zoomPercent:2] (100 = 1.0x)
                    int zoomPct = in.readUnsignedShort();
                    currentCamZoom = zoomPct / 100.0f;
                    applyZoomAndRepeat(false);
                } else if (type == 10) {
                    // CAMERA_FOCUS
                    applyZoomAndRepeat(true);
                }
            }
        } catch (Throwable ignored) {
        } finally {
            running = false;
            restoreDeviceSettings();
            try { sock.close(); } catch (IOException ignored) {}
        }
    }
}
