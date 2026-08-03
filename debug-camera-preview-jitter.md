# Camera Preview Jitter Debug

Status: OPEN

## Symptom

After adding a camera keyframe, the preview camera oscillates between the keyframe position and the recorded replay player position.

## Expected

The keyframe captures the position, rotation, and FOV currently visible in the game preview. While a camera preview is active, the sampled camera transform remains authoritative for the preview.

## Hypotheses

1. Replay packet application moves the replay player after the editor applies its sampled camera transform.
2. Camera preview applies in the client-update phase while replay progression applies later in the level sub-tick phase.
3. A newly added keyframe is initialized from defaults or stale project data rather than the currently visible preview transform.
4. The requested seek tick and the sampled timeline tick diverge during preview updates.

## Evidence Plan

Record replay tick, selected preview camera, sampled transform, replay-player transform before and after camera application, and keyframe capture source values.
