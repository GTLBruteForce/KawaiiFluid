# Volume + Emitter User Guide

## Introduction

The KawaiiFluid System provides GPU-accelerated fluid simulation in Unreal Engine. This guide covers the two main actors:

- **Kawaii Fluid Volume:** The simulation domain that contains and renders fluid particles
- **Kawaii Fluid Emitter:** Spawns particles into a Volume

## Quick Start

Follow these steps to create your first fluid simulation:

### Step 1: Place a Kawaii Fluid Volume

Drag **Kawaii Fluid Volume** from the Place Actors panel into your level.

<video controls width="100%">
  <source src="media/volume-emitter-step1.mp4" type="video/mp4">
  Your browser does not support the video tag.
</video>

### Step 2: Assign a Preset

Select the Volume and assign a **Preset** in the Details panel under *Fluid Volume > Preset*. Default preset is `DA_KF_Water`.

<video controls width="100%">
  <source src="media/volume-emitter-step2.mp4" type="video/mp4">
  Your browser does not support the video tag.
</video>

### Step 3: Place a Kawaii Fluid Emitter

Drag **Kawaii Fluid Emitter** into the level, positioned inside the Volume bounds.

<video controls width="100%">
  <source src="media/volume-emitter-step3.mp4" type="video/mp4">
  Your browser does not support the video tag.
</video>

### Step 4: Connect Emitter to Volume

Select the Emitter and set **Target Volume** to your Volume actor. If not set, it automatically finds the nearest volume at BeginPlay.

<video controls width="100%">
  <source src="media/volume-emitter-step4.mp4" type="video/mp4">
  Your browser does not support the video tag.
</video>

### Step 5: Play in Editor

Press **Play** to see the fluid simulation in action!

<video controls width="100%">
  <source src="media/volume-emitter-step5.mp4" type="video/mp4">
  Your browser does not support the video tag.
</video>
