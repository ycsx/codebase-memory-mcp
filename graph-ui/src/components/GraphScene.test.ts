import { describe, expect, it } from "vitest";
import * as THREE from "three";
import { fitCameraTargetToAspect, GRAPH_CANVAS_DPR } from "./GraphScene";

describe("GraphScene render limits", () => {
  it("caps the high-DPI WebGL backing store below the MSAA failure range", () => {
    expect(GRAPH_CANVAS_DPR[0]).toBe(1);
    expect(GRAPH_CANVAS_DPR[1]).toBeLessThanOrEqual(1.5);
  });

  it("backs the camera away for portrait canvases without moving its focus", () => {
    const target = {
      position: new THREE.Vector3(20, 15, 100),
      lookAt: new THREE.Vector3(0, 0, 0),
    };

    const fitted = fitCameraTargetToAspect(target, 0.5);

    expect(fitted.lookAt.toArray()).toEqual([0, 0, 0]);
    expect(fitted.position.distanceTo(fitted.lookAt)).toBeCloseTo(
      target.position.distanceTo(target.lookAt) * 2,
    );
    expect(target.position.toArray()).toEqual([20, 15, 100]);
  });
});
