/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Android Auto branch. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 */
package mst2.clusternav;

import java.lang.reflect.Field;

import de.vw.mib.asl.framework.api.framework.ASLFrameworkFactory;
import de.vw.mib.bap.mqbab2.common.api.APIFactory;
import de.vw.mib.bap.mqbab2.common.api.androidauto.AndroidAutoASLDataAdapter;
import de.vw.mib.bap.mqbab2.common.api.androidauto.AndroidAutoService;
import de.vw.mib.bap.mqbab2.common.api.androidauto.AndroidAutoServiceListener;

/**
 * The cluster's view of Android Auto while the phone draws or guides on the cluster: the
 * counterpart of ClusterCarPlay.
 *
 * BAP NavSD InfoStates (getCurrentBAPInfoState, H29.319.93) sends 6, "navigation in mobile
 * device", when the CarPlay, the Android Auto or the CarLife service reports route guidance.
 * For Android Auto that is ASL property 2910005, which the stock AndroidAutoASLDataAdapter reads
 * and listens to; InfoStates is the only reader of isAndroidAutoRouteGuidanceActive and asks
 * APIFactory for the service on every update. So while the phone owns the cluster this delegate
 * stands in the factory's slot and answers false; listeners stay with the stock adapter.
 * Nothing is replaced when Android Auto is not coded (the factory holds its dummy then).
 */
final class ClusterAndroidAuto implements AndroidAutoService {
    private static final int ROUTE_GUIDANCE =
        AndroidAutoServiceListener.ANDROID_AUTO_SERVICE__ANDROID_AUTO_ROUTE_GUIDANCE_ACTIVE;

    private static Field slot;
    private static AndroidAutoService stock;
    private static volatile boolean phoneOwnsCluster;

    private ClusterAndroidAuto() {
    }

    /** Puts the delegate into APIFactory, on the ASL thread like the stock factory calls. */
    static void install() {
        safeEnqueue(new Runnable() {
            public void run() {
                try {
                    Field[] fields = APIFactory.class.getDeclaredFields();
                    for (int i = 0; i < fields.length; i++) {
                        if (fields[i].getType() == AndroidAutoService.class) {
                            slot = fields[i];
                        }
                    }
                    slot.setAccessible(true);
                    /* The stock getter creates the service if nobody has asked yet. */
                    AndroidAutoService current = APIFactory.getAPIFactory().getAndroidAutoService();
                    if (!(current instanceof AndroidAutoASLDataAdapter)) {
                        Activator.log("Android Auto not coded, cluster notice left alone");
                        return;
                    }
                    stock = current;
                    slot.set(null, new ClusterAndroidAuto());
                    Activator.log("Android Auto route guidance hidden from the cluster while the phone owns it");
                } catch (Throwable t) {
                    Activator.log("cannot take the Android Auto service: " + t);
                }
            }
        });
    }

    static void uninstall() {
        safeEnqueue(new Runnable() {
            public void run() {
                try {
                    /* stock stays set: a caller may still hold the delegate. */
                    phoneOwnsCluster = false;
                    if (stock != null) {
                        slot.set(null, stock);
                    }
                } catch (Throwable t) {
                    Activator.log("cannot restore the Android Auto service: " + t);
                }
            }
        });
    }

    /** The phone's cluster video is running, or its guidance feeds the arrows. */
    static void setPhoneOwnsCluster(boolean owns) {
        if (owns == phoneOwnsCluster) {
            return;
        }
        phoneOwnsCluster = owns;
        safeEnqueue(new Runnable() {
            public void run() {
                /* The stock notification InfoStates listens to (a change of the property). */
                if (stock != null) {
                    ((AndroidAutoASLDataAdapter)stock).datapoolValueChanged(ROUTE_GUIDANCE);
                }
            }
        });
    }

    /* Callers are the bundle's own threads: an ASL failure is logged, never thrown at them. */
    private static void safeEnqueue(Runnable r) {
        try {
            ASLFrameworkFactory.getASLFrameworkAPI().getServices().getASLThreadSwitch().enqueue(r);
        } catch (Throwable t) {
            Activator.log("ASL thread not available: " + t);
        }
    }

    public boolean isAndroidAutoRouteGuidanceActive() {
        return !phoneOwnsCluster && stock.isAndroidAutoRouteGuidanceActive();
    }

    public void addAndroidAutoServiceListener(AndroidAutoServiceListener listener, int[] ids) {
        stock.addAndroidAutoServiceListener(listener, ids);
    }

    public void removeAndroidAutoServiceListener(AndroidAutoServiceListener listener, int[] ids) {
        stock.removeAndroidAutoServiceListener(listener, ids);
    }
}
