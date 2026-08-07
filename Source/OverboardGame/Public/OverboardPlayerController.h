// OverboardPlayerController.h
//
// W2 (overboard#162): the real stick-to-channel mapping, deadzone/curve shaping, and a
// per-frame send.
//
// THE CONTROL MODE MATTERS FOR HOW THIS READS, and the CEO's ruling on it reversed mid-W2: the
// outer velocity loop is OFF. The board does NOT station-keep -- there is no term pulling it back
// to rest. Centre stick means "coast at whatever velocity the board already has," not "hold
// position." Fore/aft is a LEAN command, not a ground-speed command: push forward -> lean ->
// accelerate; pull back -> decelerate, then reverse. That's real onewheel behaviour, deliberate,
// not a placeholder. Because there's no active loop fighting a centred stick, a small uncorrected
// centre-stick value is far less consequential than it would be under station-keeping (no
// permanent creep-to-drift mechanism -- it just adds a little unwanted lean, not an unbounded
// position error) -- still worth a sensible deadzone, but not something to over-engineer here.
// This class's only job is to hand the host a clean, deadzoned, clamped [-1,1] command every
// frame; it does no station-keeping (or any other physics) itself, regardless of control mode.
//
// Mapping (per #162's W2 dispatch):
//   left stick fore/aft (Gamepad_LeftY)  -> weight_shift_fore_aft (a LEAN command -- accelerates/
//                                            decelerates; centre stick coasts, it does not brake)
//   right stick lateral (Gamepad_RightX) -> weight_shift_lateral AND steer, both from the same
//                                            physical axis (lean-to-steer: leaning is how you
//                                            steer, so one stick axis legitimately drives both
//                                            wire channels)
//
// steer is explicitly a NON-PHYSICAL game channel (the simulated wheel is a cylinder and cannot
// carve) -- see the wire spec and the repo README's "what is deliberately not physical" section.
// Nothing produced via this channel may be presented as a controls result.
//
// KEYBOARD (overboard#162, CEO ask: "I want the driven mode to work on keyboard first"):
// added ALONGSIDE the gamepad bindings above, in the same mapping context -- the gamepad path is
// untouched and stays the launch configuration.
//   W / Up Arrow    -> weight_shift_fore_aft positive   S / Down Arrow -> negative
//   D / Right Arrow -> weight_shift_lateral + steer positive   A / Left Arrow -> negative
//   Space           -> arm                               R              -> reset
// Each axis is two digital key mappings (one with a Negate modifier) feeding the same Axis1D
// action the analogue stick does -- Enhanced Input combines whichever source is active. A raw
// digital key is 0 or +-1 the instant it's pressed/released, unlike a stick's gradual sweep, so
// SendInputPacket ramps toward whatever the mapped value is (see SmoothedForeAft etc. and
// KeyboardRampSpeed below) rather than sending a snap to full lean -- gamepad input already
// arrives gradually, so the same ramp is a no-op for it in practice.
//
// ENGAGE/DISENGAGE + STATIONARY YAW-AIM (overboard-game#28):
//
//   IA_Arm already IS the "press start / engage" binding -- Space and gamepad A were mapped to
//   it since #162, before this feature existed. Nothing new to bind: pressing it sends
//   OverboardWire::EInputFlags::Arm, exactly as before; whether that actually engages the motor is
//   entirely `sim-host`'s call (this class never predicts it -- see EngageWireMapping.h).
//
//   The right-stick-X / A-D / Left-Right axis that already drives `steer` for riding turn now
//   ALSO carries the stationary yaw-aim command while the board is disengaged -- SendInputPacket
//   sends the identical shaped value either way, unconditionally, and it is entirely `sim-host`'s
//   job to decide which meaning `steer` carries at a given instant (engaged/moving -> riding turn;
//   disengaged -> pivot in place). See EngageWireMapping.h for why that is safe and why gating the
//   SEND here would either be a no-op or would regress riding turn.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "OverboardPlayerController.generated.h"

class UInputMappingContext;
class UInputAction;
struct FInputActionValue;
class FSocket;
class FInternetAddr;

UCLASS()
class OVERBOARDGAME_API AOverboardPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AOverboardPlayerController();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void SetupInputComponent() override;
	virtual void PlayerTick(float DeltaTime) override;

	// THE FIX (overboard#162): AddMappingContext used to be called from SetupInputComponent, and
	// silently did nothing if GetLocalPlayer() was null there -- which it reliably was in -game
	// standalone (less reliably in PIE, which is why this shipped once already and looked fine).
	// No mapping context added means no key is ever mapped to any action, every handler stays at
	// zero, and NOTHING was logged -- a keyboard AND gamepad went dark with no error anywhere.
	// ReceivedPlayer() is the engine's own purpose-built hook: called right after Player is
	// assigned, guaranteed non-null for a local player at that point. Action/binding
	// CONSTRUCTION stays in SetupInputComponent (InputComponent does exist by then); only
	// registering the mapping context moves.
	virtual void ReceivedPlayer() override;

protected:
	// Below this magnitude (post-normalization, [0,1] range) a raw stick reads as zero. Sized
	// comfortably above typical Xbox/PS5 stick centre noise (a couple of percent) so a centred
	// stick doesn't add unwanted lean. Deliberately not over-engineered: with the outer velocity
	// loop off, a centred-stick miss just means a little unrequested lean, not a runaway
	// position error, so this is a sensible-default deadzone, not a load-bearing one. Tune by
	// feel once someone can actually drive it.
	UPROPERTY(EditAnywhere, Category = "Board|Input", meta = (ClampMin = "0.0", ClampMax = "0.9"))
	float StickDeadzone = 0.12f;

	// Response curve applied past the deadzone: output = sign(x) * rescaled(x) ^ ResponseCurveExponent,
	// where rescaled(x) remaps [Deadzone, 1] back to [0, 1] so the curve starts at 0 right past the
	// deadzone edge instead of jumping. 1.0 = linear. >1.0 gives finer control near centre (small
	// stick deflection -> even smaller command) while still reaching +-1 at full deflection. This
	// now shapes a LEAN command (which maps to acceleration), not a ground-speed command -- it
	// will feel different from a station-keeping curve. 2.0 is a starting point, not a measured
	// value; tune by feel once someone has actually driven it (see PR).
	UPROPERTY(EditAnywhere, Category = "Board|Input", meta = (ClampMin = "1.0", ClampMax = "4.0"))
	float ResponseCurveExponent = 2.0f;

	// FInterpTo speed ramping the raw mapped axis value toward what SendInputPacket actually
	// sends (see SmoothedForeAft etc.). A keyboard key is digital -- 0 or +-1 the instant it's
	// pressed or released -- and "an analogue stick moves gradually, and the whole control feel
	// depends on that" (overboard#162 CEO dispatch), so this exists specifically to keep a
	// keyboard-driven lean from slamming to full deflection in one frame. Higher = snappier,
	// lower = more gradual. First guess, not a measured value -- tune by feel once someone has
	// actually driven it with a keyboard.
	UPROPERTY(EditAnywhere, Category = "Board|Input", meta = (ClampMin = "0.5", ClampMax = "20.0"))
	float KeyboardRampSpeed = 3.0f;

private:
	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> MappingContext;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_WeightShiftForeAft;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_WeightShiftLateral;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Steer; // NON-PHYSICAL game channel
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Arm;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Reset;

	// overboard#162: "The window traps input with no way out" -- the CEO had to switch virtual
	// desktops to force-quit. Not a launch-blocker by itself, but anything handed to him to play
	// with must be exitable without hunting for how, and this will bite during a capture session
	// too. Escape only; not on the gamepad (no natural "quit" button to reuse without stealing
	// one of the four already-mapped face buttons).
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Quit;

	// Raw, unshaped axis state as last reported by Enhanced Input -- the TARGET the ramp below
	// chases, not what gets sent. For the gamepad this already moves gradually; for the keyboard
	// this snaps between 0 and +-1 the instant a key is pressed/released.
	float CurrentForeAft = 0.f;
	float CurrentLateral = 0.f;
	float CurrentSteer = 0.f;
	bool bArmHeld = false;
	bool bResetHeld = false;

	// Ramped toward Current* each PlayerTick via KeyboardRampSpeed, so a digital keyboard press
	// ramps in rather than snapping to full deflection. This -- not Current* directly -- is what
	// SendInputPacket passes through ShapeAxis (deadzone/curve) and sends.
	float SmoothedForeAft = 0.f;
	float SmoothedLateral = 0.f;
	float SmoothedSteer = 0.f;

	FSocket* SendSocket = nullptr;
	TSharedPtr<FInternetAddr> HostAddr;
	uint64 SendSeq = 0; // monotonic; never reset while the socket is open, so the host can detect loss

	// Reset-on-fall (overboard#162 W3): with the outer velocity loop off, a fallen board coasts
	// on whatever velocity it had rather than stopping, so it will leave the play area if nobody
	// reacts. Rising-edge on ABoardActor::IsFallen() -- fires Flags::Reset for exactly one
	// packet, not held, so a manual Reset button press afterwards isn't fighting a stuck flag.
	bool bWasFallenLastTick = false;
	bool bAutoResetPending = false;
	void CheckForAutoResetOnFall();

	void OnWeightShiftForeAft(const FInputActionValue& Value);
	void OnWeightShiftLateral(const FInputActionValue& Value);
	void OnSteer(const FInputActionValue& Value);
	void OnArm(const FInputActionValue& Value);
	void OnReset(const FInputActionValue& Value);
	void OnQuit(const FInputActionValue& Value);

	// Deadzone + response curve, applied to a raw stick axis already in [-1,1]. Does not clamp
	// its own output beyond what the curve already guarantees -- SendInputPacket clamps again
	// right before the wire regardless, per the "do not rely on the host to sanitise" rule.
	float ShapeAxis(float Raw) const;

	void SendInputPacket();

	// Headless verification for the AddMappingContext fix above (overboard#162): this
	// environment has no display or real keyboard/gamepad to press a key with. Enabled only by
	// the -OverboardInputSelfTest command-line switch; otherwise this never runs and costs
	// nothing. Injects a real "W pressed" event through UPlayerInput::InputKey
	// (FInputKeyEventArgs::CreateSimulated -- an engine-provided mechanism for exactly this, not
	// a hack), which exercises the ACTUAL Enhanced Input pipeline end to end: mapping context
	// lookup, trigger evaluation, the Negate modifier on the paired key, our handler, the ramp,
	// and the real wire send -- then logs PASS/FAIL on whether the value actually went non-zero.
	// "Do not report this fixed until a keypress produces a non-zero value on the wire" -- this
	// is that check, run without a display.
	void RunInputSelfTest();
};
