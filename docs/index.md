# Kawaii Fluid System

Kawaii Fluid is a high-performance, GPU-accelerated SPH (Smoothed Particle Hydrodynamics) fluid simulation plugin for Unreal Engine 5.7. It provides a robust framework for real-time fluid dynamics, featuring advanced collision handling, interactive painting tools, and high-quality screen-space rendering.

## Core Architecture

The system is built upon a modular architecture divided into Runtime and Editor modules, ensuring efficiency and scalability.

### KawaiiFluidRuntime
The heart of the simulation, responsible for physics calculation and rendering.
*   **Simulation Context:** Pure stateless solver logic handling fluid behavior such as viscosity, surface tension, and density constraints.
*   **GPU Pipeline:** Utilizes Compute Shaders for parallelized particle updates, spatial hashing, and sorting.
*   **Physics Solvers:** Implementation of SPH and XPBD (Extended Position Based Dynamics) for stable and realistic fluid motion.
*   **Collision System:** Advanced collision detection supporting Static Meshes, Skeletal Meshes (via BVH), and primitive shapes.
*   **Rendering Pipeline:** A dedicated screen-space rendering pipeline including depth smoothing, normal generation, and thickness-based shading (Metaballs).

### KawaiiFluidEditor
A suite of tools designed to enhance the development workflow within the Unreal Editor.
*   **Fluid Brush Tool:** Allows artists to paint, sculpt, and erase fluid particles directly in the viewport.
*   **Asset Editor:** Dedicated editor for Fluid Preset Data Assets, featuring real-time preview and property management.
*   **Custom Details Panel:** Optimized UI for managing simulation components and volume settings.

## Technical Highlights

### GPU-Accelerated SPH Simulation
The simulation runs entirely on the GPU, allowing for tens of thousands of particles at interactive frame rates. It employs a multi-pass compute shader pipeline:
1.  **Spatial Hashing:** Efficient neighbor search using a grid-based spatial hash.
2.  **Density & Pressure:** Calculation of particle density and pressure forces to maintain incompressibility.
3.  **Viscosity & Adhesion:** Solvers for fluid internal friction and surface interaction.
4.  **Collision Feedback:** Real-time feedback and force application between fluid and scene objects.

### Advanced Rendering Techniques
To achieve a high-quality liquid look, the system uses a Screen-Space Fluid Rendering (SSFR) pipeline:
*   **Anisotropic Smoothing:** Smooths the particle depth buffer while preserving sharp edges and surface details.
*   **Metaball Rendering:** Generates a continuous surface from discrete particles.
*   **Flow Accumulation:** Simulates the visual movement and surface decoration of the fluid.

## Key Components

*   **Kawaii Fluid Volume:** Defines the 3D domain where simulation and rendering occur.
*   **Kawaii Fluid Emitter:** A modular component to spawn particles with specific velocity and rate.
*   **Kawaii Fluid Interaction Component:** Enables real-time interaction between Actors (e.g., Players) and the fluid system using Physics Asset data.
*   **Kawaii Fluid Simulator Subsystem:** A world-level subsystem that orchestrates all active simulations and manages global resources.

---

Copyright 2026 Team_Bruteforce. All Rights Reserved.