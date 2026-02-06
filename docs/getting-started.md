# Getting Started

This guide covers the essential steps to integrate Kawaii Fluid into your Unreal Engine 5.7 project.

## Installation

1.  **Plugin Setup:** Clone this repository into your project's `Plugins/KawaiiFluidSystem` folder.
2.  **Generate Files:** Right-click your `.uproject` file and select **Generate Visual Studio project files**.
3.  **Build:** Open your solution in Visual Studio 2022 and **Build** your project.

---

## The 3 Essential Rules for Success

To ensure your fluid simulation works as expected, always verify these three core components:

### 1. Define the Simulation Domain (Volume)
Every fluid simulation requires a **Kawaii Fluid Volume** Actor in the level.
*   **Assign a Preset:** In the Details panel, you MUST assign a `UKawaiiFluidPresetDataAsset` (e.g., `DA_KF_Water`). This tells the system how the fluid should behave (viscosity, density, etc.).

### 2. Setup Emitters and Obstacles
Fluid needs a source and something to collide with.
*   **Place Emitter:** Ensure your **Kawaii Fluid Emitter** is physically located **inside** the bounds of a Volume.
*   **Static Mesh Collisions:** Static Meshes inside the volume automatically act as colliders.
*   **Important:** For custom meshes, ensure they have a **Simple Collision** generated in the Static Mesh Editor. The simulation relies on these simple shapes for optimal GPU performance.

### 3. Enable Player Interaction
To make your character interact with the fluid (splashes, resistance):
*   **Add Component:** Attach a `UKawaiiFluidSimulationComponent` to your character blueprint.
*   **Auto Collider:** Within the component settings, check **"Auto Collider"**. This automatically sends your character's **Physics Asset** to the GPU, allowing particles to react to your character's movements in real-time.

---

## Quick Verification
*   Is there a **Kawaii Fluid Volume** with a **Preset** assigned?
*   Is the **Emitter** inside the Volume bounds?
*   Does your character have the **Simulation Component** with **Auto Collider** enabled?

If these three are checked, you are ready to play!