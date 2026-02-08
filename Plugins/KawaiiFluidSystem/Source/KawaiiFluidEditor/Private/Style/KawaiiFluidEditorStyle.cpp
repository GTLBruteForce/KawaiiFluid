// Copyright 2026 Team_Bruteforce. All Rights Reserved.

#include "Style/KawaiiFluidEditorStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/SlateTypes.h"
#include "Styling/CoreStyle.h"
#include "Interfaces/IPluginManager.h"
#include "SlateOptMacros.h"

#define IMAGE_BRUSH(RelativePath, ...) FSlateImageBrush(Style->RootToContentDir(RelativePath, TEXT(".png")), __VA_ARGS__)
#define BOX_BRUSH(RelativePath, ...) FSlateBoxBrush(Style->RootToContentDir(RelativePath, TEXT(".png")), __VA_ARGS__)
#define BORDER_BRUSH(RelativePath, ...) FSlateBorderBrush(Style->RootToContentDir(RelativePath, TEXT(".png")), __VA_ARGS__)
#define IMAGE_BRUSH_SVG(RelativePath, ...) FSlateVectorImageBrush(Style->RootToContentDir(RelativePath, TEXT(".svg")), __VA_ARGS__)

TSharedPtr<FSlateStyleSet> FKawaiiFluidEditorStyle::StyleInstance = nullptr;

void FKawaiiFluidEditorStyle::Initialize()
{
	if (!StyleInstance.IsValid())
	{
		StyleInstance = Create();
		FSlateStyleRegistry::RegisterSlateStyle(*StyleInstance);
	}
}

void FKawaiiFluidEditorStyle::Shutdown()
{
	FSlateStyleRegistry::UnRegisterSlateStyle(*StyleInstance);
	ensure(StyleInstance.IsUnique());
	StyleInstance.Reset();
}

void FKawaiiFluidEditorStyle::ReloadTextures()
{
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().GetRenderer()->ReloadTextureResources();
	}
}

const ISlateStyle& FKawaiiFluidEditorStyle::Get()
{
	return *StyleInstance;
}

FName FKawaiiFluidEditorStyle::GetStyleSetName()
{
	static FName StyleSetName(TEXT("FluidEditorStyle"));
	return StyleSetName;
}

const FSlateBrush* FKawaiiFluidEditorStyle::GetBrush(FName PropertyName)
{
	return StyleInstance->GetBrush(PropertyName);
}

TSharedRef<FSlateStyleSet> FKawaiiFluidEditorStyle::Create()
{
	TSharedRef<FSlateStyleSet> Style = MakeShareable(new FSlateStyleSet(GetStyleSetName()));
	Style->SetContentRoot(IPluginManager::Get().FindPlugin(TEXT("KawaiiFluidSystem"))->GetContentDir());

	const FVector2D Icon16x16(16.0f, 16.0f);
	const FVector2D Icon20x20(20.0f, 20.0f);
	const FVector2D Icon40x40(40.0f, 40.0f);
	const FVector2D Icon64x64(64.0f, 64.0f);

	// Class icons - using default editor style icons as fallback
	Style->Set("ClassIcon.KawaiiFluidPresetDataAsset", new FSlateRoundedBoxBrush(FLinearColor(0.2f, 0.4f, 0.8f, 1.0f), 4.0f, FLinearColor::Transparent, 0.0f, Icon16x16));
	Style->Set("ClassThumbnail.KawaiiFluidPresetDataAsset", new FSlateRoundedBoxBrush(FLinearColor(0.2f, 0.4f, 0.8f, 1.0f), 8.0f, FLinearColor::Transparent, 0.0f, Icon64x64));

	// Toolbar icons - using simple colored boxes as placeholders
	Style->Set("FluidEditor.Play", new FSlateRoundedBoxBrush(FLinearColor(0.2f, 0.8f, 0.2f, 1.0f), 4.0f, FLinearColor::Transparent, 0.0f, Icon20x20));
	Style->Set("FluidEditor.Pause", new FSlateRoundedBoxBrush(FLinearColor(0.8f, 0.6f, 0.2f, 1.0f), 4.0f, FLinearColor::Transparent, 0.0f, Icon20x20));
	Style->Set("FluidEditor.Stop", new FSlateRoundedBoxBrush(FLinearColor(0.8f, 0.2f, 0.2f, 1.0f), 4.0f, FLinearColor::Transparent, 0.0f, Icon20x20));
	Style->Set("FluidEditor.Reset", new FSlateRoundedBoxBrush(FLinearColor(0.4f, 0.6f, 0.8f, 1.0f), 4.0f, FLinearColor::Transparent, 0.0f, Icon20x20));

	// Tab icons
	Style->Set("FluidEditor.Tabs.Viewport", new FSlateRoundedBoxBrush(FLinearColor(0.3f, 0.5f, 0.7f, 1.0f), 4.0f, FLinearColor::Transparent, 0.0f, Icon16x16));
	Style->Set("FluidEditor.Tabs.Details", new FSlateRoundedBoxBrush(FLinearColor(0.5f, 0.3f, 0.7f, 1.0f), 4.0f, FLinearColor::Transparent, 0.0f, Icon16x16));
	Style->Set("FluidEditor.Tabs.PreviewSettings", new FSlateRoundedBoxBrush(FLinearColor(0.7f, 0.5f, 0.3f, 1.0f), 4.0f, FLinearColor::Transparent, 0.0f, Icon16x16));

	return Style;
}

#undef IMAGE_BRUSH
#undef BOX_BRUSH
#undef BORDER_BRUSH
#undef IMAGE_BRUSH_SVG
