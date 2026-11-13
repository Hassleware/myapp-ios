// The iPhone's tap motor (the Taptic Engine). SDL has no call for it, so this small file asks UIKit directly.
#import <UIKit/UIKit.h>

extern "C" void gamelab_haptic(int kind) {
    // 0 light, 1 medium, 2 heavy: a single tap. 3 success, 4 warning, 5 error: the system's patterns.
    if (kind >= 3) {
        UINotificationFeedbackGenerator* generator = [[UINotificationFeedbackGenerator alloc] init];
        UINotificationFeedbackType type = kind == 3 ? UINotificationFeedbackTypeSuccess
                                        : kind == 4 ? UINotificationFeedbackTypeWarning
                                                    : UINotificationFeedbackTypeError;
        [generator notificationOccurred:type];
    } else {
        UIImpactFeedbackStyle style = kind == 0 ? UIImpactFeedbackStyleLight
                                    : kind == 2 ? UIImpactFeedbackStyleHeavy
                                                : UIImpactFeedbackStyleMedium;
        UIImpactFeedbackGenerator* generator = [[UIImpactFeedbackGenerator alloc] initWithStyle:style];
        [generator impactOccurred];
    }
}
