// UI component editors (Framework/ui) for the Properties inspector. Included by
// imcomponentinspectors.cpp inside its anonymous namespace, so it shares that file's
// helpers: Edits (one undo step per widget interaction), FlagCheckbox, DragAngle,
// EntityDropField, AssetDropField, EditString, HelpMarker.

template <typename E>
bool EnumCombo(const char* label, E& value, const char* const* names, int count)
{
	int v = (int)value;
	if (!ImGui::Combo(label, &v, names, count)) return false;
	value = (E)v;
	return true;
}

bool BitCheckbox(const char* label, uint32_t& flags, uint32_t bit)
{
	bool v = (flags & bit) != 0;
	if (!ImGui::Checkbox(label, &v)) return false;
	if (v) flags |= bit;
	else flags &= ~bit;
	return true;
}

// Unity's anchor presets: rows top / middle / bottom / stretch, columns left / centre /
//	right / stretch. Picking one sets anchors and pivot and puts the element at the anchor
//	(position 0; a stretch keeps no extra size).
bool AnchorPresets(st::ui::UIRectComponent& r)
{
	bool changed = false;
	if (ImGui::Button("Anchor presets..."))
		ImGui::OpenPopup("##anchor_presets");
	if (ImGui::BeginPopup("##anchor_presets"))
	{
		static const char* rows[] = { "top", "middle", "bottom", "stretch" };
		static const char* cols[] = { "left", "centre", "right", "stretch" };
		const float at[3] = { 0.0f, 0.5f, 1.0f };
		ImGui::TextDisabled("anchors + pivot (position reset)");
		for (int row = 0; row < 4; ++row)
		{
			for (int col = 0; col < 4; ++col)
			{
				char id[64];
				snprintf(id, sizeof(id), "%s\n%s##%d%d", rows[row], cols[col], row, col);
				if (col > 0) ImGui::SameLine();
				if (ImGui::Button(id, ImVec2(64, 40)))
				{
					const bool stretchX = col == 3, stretchY = row == 3;
					r.anchorMin.x = stretchX ? 0.0f : at[col];
					r.anchorMax.x = stretchX ? 1.0f : at[col];
					r.anchorMin.y = stretchY ? 0.0f : at[row];
					r.anchorMax.y = stretchY ? 1.0f : at[row];
					r.pivot.x = stretchX ? 0.5f : at[col];
					r.pivot.y = stretchY ? 0.5f : at[row];
					if (stretchX) r.size.x = 0;
					if (stretchY) r.size.y = 0;
					r.position = XMFLOAT2(0, 0);
					changed = true;
					ImGui::CloseCurrentPopup();
				}
			}
		}
		ImGui::EndPopup();
	}
	return changed;
}

void DrawUICanvas(Scene& scene, Entity e, st::EditorHistory* history)
{
	st::ui::UICanvasComponent* c = st::ui::Canvases(scene).GetComponent(e);
	if (!c) return;
	if (!ComponentHeader(scene, e, st::ui::kCanvases, "UI Canvas", true, history)) return;
	const Edits track{ scene, e, history };

	static const char* modes[] = { "Screen (over the game view)", "World (on this object's material)" };
	EnumCombo("Render mode", c->renderMode, modes, 2);
	track("Set Canvas Render Mode");
	static const char* scales[] = { "Constant pixel size", "Scale with screen size" };
	EnumCombo("Scale mode", c->scaleMode, scales, 2);
	track("Set Canvas Scale Mode");
	ImGui::DragFloat2(c->renderMode == st::ui::UICanvasComponent::RenderMode::World ? "Texture size" : "Reference resolution",
		&c->referenceResolution.x, 1.0f, 16.0f, 8192.0f, "%.0f");
	track("Set Canvas Resolution");
	if (c->scaleMode == st::ui::UICanvasComponent::ScaleMode::ScaleWithScreen &&
		c->renderMode == st::ui::UICanvasComponent::RenderMode::Screen)
	{
		ImGui::SliderFloat("Match width / height", &c->match, 0.0f, 1.0f);
		track("Set Canvas Match");
	}
	else
	{
		ImGui::DragFloat("Scale factor", &c->scaleFactor, 0.01f, 0.05f, 20.0f);
		track("Set Canvas Scale Factor");
	}
	ImGui::DragInt("Sort order", &c->sortOrder);
	track("Set Canvas Sort Order");
	FlagCheckbox("Hidden", [&] { return c->IsHidden(); }, [&](bool v) { c->SetHidden(v); });
	track("Hide Canvas");
	ImGui::TextDisabled("now: %.0f x %.0f units, %.3f px/unit", c->_size.x, c->_size.y, c->_scale);
}

void DrawUIRect(Scene& scene, Entity e, st::EditorHistory* history)
{
	st::ui::UIRectComponent* r = st::ui::Rects(scene).GetComponent(e);
	if (!r) return;
	if (!ComponentHeader(scene, e, st::ui::kRects, "UI Rect", true, history)) return;
	const Edits track{ scene, e, history };

	if (st::ui::Canvases(scene).Contains(e))
	{
		ImGui::TextDisabled("A canvas's rect is the whole screen / texture.");
		return;
	}
	if (AnchorPresets(*r) && history)
	{
		// The popup's button is the whole edit: record it as one step after the fact.
		history->BeginEntity(scene, e, "Anchor Preset");
		history->Commit(scene);
	}
	const bool stretchX = r->anchorMin.x != r->anchorMax.x;
	const bool stretchY = r->anchorMin.y != r->anchorMax.y;
	ImGui::DragFloat2("Position", &r->position.x, 0.5f);
	track("Move UI Element");
	ImGui::DragFloat2(stretchX || stretchY ? "Size (extra over anchors)" : "Size", &r->size.x, 0.5f);
	track("Resize UI Element");
	ImGui::DragFloat2("Anchor min", &r->anchorMin.x, 0.005f, 0.0f, 1.0f);
	track("Set Anchor Min");
	ImGui::DragFloat2("Anchor max", &r->anchorMax.x, 0.005f, 0.0f, 1.0f);
	track("Set Anchor Max");
	ImGui::DragFloat2("Pivot", &r->pivot.x, 0.005f, 0.0f, 1.0f);
	track("Set Pivot");
	HelpMarker("Fractions of the parent: (0,0) top left, (1,1) bottom right. Equal anchors = fixed size at "
		"that point; split anchors stretch with the parent and Size is the extra (negative = inset).");
	DragAngle("Rotation", r->rotation);
	track("Rotate UI Element");
	ImGui::DragFloat2("Scale", &r->scale.x, 0.01f, 0.0f, 100.0f);
	track("Scale UI Element");
	ImGui::DragInt("Order", &r->order);
	track("Set UI Order");
	HelpMarker("Among siblings: higher draws later (on top) and is hit first.");
	ImGui::SliderFloat("Opacity", &r->opacity, 0.0f, 1.0f);
	track("Set UI Opacity");
	BitCheckbox("Hidden", r->_flags, st::ui::UIRectComponent::HIDDEN);
	track("Hide UI Element");
	ImGui::SameLine();
	BitCheckbox("Clip children", r->_flags, st::ui::UIRectComponent::CLIP_CHILDREN);
	track("Set UI Clip");
	BitCheckbox("Ignore layout", r->_flags, st::ui::UIRectComponent::IGNORE_LAYOUT);
	track("Set UI Ignore Layout");
	ImGui::SameLine();
	BitCheckbox("Block pointer", r->_flags, st::ui::UIRectComponent::BLOCK_POINTER);
	track("Set UI Block Pointer");
	ImGui::TextDisabled("rect %.0f, %.0f  %.0f x %.0f%s", r->_rect.x, r->_rect.y, r->_rect.z, r->_rect.w,
		r->_laidOut ? "  (placed by parent layout)" : "");
}

void DrawUIImage(Scene& scene, Entity e, st::EditorHistory* history)
{
	st::ui::UIImageComponent* i = st::ui::Images(scene).GetComponent(e);
	if (!i) return;
	if (!ComponentHeader(scene, e, st::ui::kImages, "UI Image", true, history)) return;
	const Edits track{ scene, e, history };

	AssetDropField("Texture", i->textureName);
	track("Set UI Image Texture");
	ImGui::ColorEdit4("Color", &i->color.x);
	track("Set UI Image Color");
	static const char* types[] = { "Simple", "Sliced (9-slice)", "Filled horizontal", "Filled vertical" };
	EnumCombo("Type", i->type, types, 4);
	track("Set UI Image Type");
	if (i->type == st::ui::UIImageComponent::Type::Sliced)
	{
		ImGui::DragFloat4("Border L T R B", &i->border.x, 0.5f, 0.0f, 4096.0f, "%.0f");
		track("Set UI Image Border");
		ImGui::DragFloat("Border scale", &i->borderScale, 0.01f, 0.01f, 20.0f);
		track("Set UI Image Border Scale");
	}
	if (i->type == st::ui::UIImageComponent::Type::FilledHorizontal ||
		i->type == st::ui::UIImageComponent::Type::FilledVertical)
	{
		ImGui::SliderFloat("Fill amount", &i->fillAmount, 0.0f, 1.0f);
		track("Set UI Image Fill");
		BitCheckbox("Fill from the other end", i->_flags, st::ui::UIImageComponent::FILL_REVERSE);
		track("Set UI Image Fill Direction");
	}
	ImGui::DragFloat("Corner radius", &i->cornerRadius, 0.25f, 0.0f, 1000.0f);
	track("Set UI Image Corners");
	static const char* gradients[] = { "None", "Horizontal", "Vertical" };
	EnumCombo("Gradient", i->gradient, gradients, 3);
	track("Set UI Image Gradient");
	if (i->gradient != st::ui::UIImageComponent::Gradient::None)
	{
		ImGui::ColorEdit4("Gradient to", &i->gradientColor.x);
		track("Set UI Image Gradient Color");
	}
	BitCheckbox("Preserve aspect", i->_flags, st::ui::UIImageComponent::PRESERVE_ASPECT);
	track("Set UI Image Aspect");
	ImGui::SameLine();
	BitCheckbox("Raycast target", i->_flags, st::ui::UIImageComponent::RAYCAST_TARGET);
	track("Set UI Image Raycast");
}

void DrawUIText(Scene& scene, Entity e, st::EditorHistory* history)
{
	st::ui::UITextComponent* t = st::ui::Texts(scene).GetComponent(e);
	if (!t) return;
	if (!ComponentHeader(scene, e, st::ui::kTexts, "UI Text", true, history)) return;
	const Edits track{ scene, e, history };

	static char buf[4096];
	snprintf(buf, sizeof(buf), "%s", t->text.c_str());
	if (ImGui::InputTextMultiline("Text", buf, sizeof(buf), ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 3.5f)))
		t->text = buf;
	track("Set UI Text");
	AssetDropField("Font", t->fontName);
	track("Set UI Font");
	HelpMarker(".ttf / .otf asset; empty = the engine's font.");
	ImGui::DragFloat("Size", &t->size, 0.25f, 1.0f, 1000.0f);
	track("Set UI Text Size");
	ImGui::ColorEdit4("Color", &t->color.x);
	track("Set UI Text Color");
	static const char* hal[] = { "Left", "Centre", "Right" };
	static const char* val[] = { "Top", "Middle", "Bottom" };
	EnumCombo("Horizontal", t->hAlign, hal, 3);
	track("Set UI Text Align");
	EnumCombo("Vertical", t->vAlign, val, 3);
	track("Set UI Text Align");
	BitCheckbox("Wrap", t->_flags, st::ui::UITextComponent::WRAP);
	track("Set UI Text Wrap");
	ImGui::SameLine();
	BitCheckbox("Shadow", t->_flags, st::ui::UITextComponent::SHADOW);
	track("Set UI Text Shadow");
	ImGui::SameLine();
	BitCheckbox("Raycast target", t->_flags, st::ui::UITextComponent::RAYCAST_TARGET);
	track("Set UI Text Raycast");
	if (t->_flags & st::ui::UITextComponent::SHADOW)
	{
		ImGui::ColorEdit4("Shadow color", &t->shadowColor.x);
		track("Set UI Text Shadow");
		ImGui::DragFloat2("Shadow offset", &t->shadowOffset.x, 0.1f);
		track("Set UI Text Shadow");
	}
	ImGui::SliderFloat("Bolden", &t->bolden, 0.0f, 1.0f);
	track("Set UI Text Bolden");
	ImGui::SliderFloat("Softness", &t->softness, 0.0f, 1.0f);
	track("Set UI Text Softness");
	ImGui::DragFloat2("Spacing (char, line)", &t->spacing.x, 0.1f);
	track("Set UI Text Spacing");
}

void EventFields(Scene& scene, const char* label, std::string& handler, Entity& target, const Edits& track)
{
	EditString(label, handler);
	track("Set UI Event");
	EntityDropField(scene, "Event target", target);
	track("Set UI Event Target");
	HelpMarker("A function of that name runs in every stScript on the event target (this element when "
		"unset). Play mode only.");
}

void DrawUIButton(Scene& scene, Entity e, st::EditorHistory* history)
{
	st::ui::UIButtonComponent* b = st::ui::Buttons(scene).GetComponent(e);
	if (!b) return;
	if (!ComponentHeader(scene, e, st::ui::kButtons, "UI Button", true, history)) return;
	const Edits track{ scene, e, history };

	FlagCheckbox("Interactable", [&] { return b->IsInteractable(); }, [&](bool v) { b->SetInteractable(v); });
	track("Set Button Interactable");
	EntityDropField(scene, "Target graphic", b->targetGraphic);
	track("Set Button Graphic");
	ImGui::ColorEdit4("Normal", &b->normalColor.x);
	track("Set Button Color");
	ImGui::ColorEdit4("Hover", &b->hoverColor.x);
	track("Set Button Color");
	ImGui::ColorEdit4("Pressed", &b->pressedColor.x);
	track("Set Button Color");
	ImGui::ColorEdit4("Disabled", &b->disabledColor.x);
	track("Set Button Color");
	ImGui::DragFloat("Fade (s)", &b->fadeDuration, 0.005f, 0.0f, 2.0f);
	track("Set Button Fade");
	EventFields(scene, "On click", b->onClick, b->eventTarget, track);
	ImGui::TextDisabled("func %s(GameObject sender)", b->onClick.empty() ? "<name>" : b->onClick.c_str());
}

void DrawUIToggle(Scene& scene, Entity e, st::EditorHistory* history)
{
	st::ui::UIToggleComponent* t = st::ui::Toggles(scene).GetComponent(e);
	if (!t) return;
	if (!ComponentHeader(scene, e, st::ui::kToggles, "UI Toggle", true, history)) return;
	const Edits track{ scene, e, history };

	BitCheckbox("Is on", t->_flags, st::ui::UIToggleComponent::IS_ON);
	track("Set Toggle");
	ImGui::SameLine();
	BitCheckbox("Interactable", t->_flags, st::ui::UIToggleComponent::INTERACTABLE);
	track("Set Toggle Interactable");
	EntityDropField(scene, "Graphic (shown when on)", t->graphic);
	track("Set Toggle Graphic");
	EventFields(scene, "On value changed", t->onValueChanged, t->eventTarget, track);
	ImGui::TextDisabled("func %s(GameObject sender, bool on)", t->onValueChanged.empty() ? "<name>" : t->onValueChanged.c_str());
}

void DrawUISlider(Scene& scene, Entity e, st::EditorHistory* history)
{
	st::ui::UISliderComponent* s = st::ui::Sliders(scene).GetComponent(e);
	if (!s) return;
	if (!ComponentHeader(scene, e, st::ui::kSliders, "UI Slider", true, history)) return;
	const Edits track{ scene, e, history };

	static const char* dirs[] = { "Left to right", "Right to left", "Bottom to top", "Top to bottom" };
	EnumCombo("Direction", s->direction, dirs, 4);
	track("Set Slider Direction");
	ImGui::DragFloat("Min", &s->minValue, 0.01f);
	track("Set Slider Range");
	ImGui::DragFloat("Max", &s->maxValue, 0.01f);
	track("Set Slider Range");
	ImGui::SliderFloat("Value", &s->value, std::min(s->minValue, s->maxValue), std::max(s->minValue, s->maxValue));
	track("Set Slider Value");
	BitCheckbox("Whole numbers", s->_flags, st::ui::UISliderComponent::WHOLE_NUMBERS);
	track("Set Slider Whole Numbers");
	ImGui::SameLine();
	BitCheckbox("Interactable", s->_flags, st::ui::UISliderComponent::INTERACTABLE);
	track("Set Slider Interactable");
	EntityDropField(scene, "Fill rect", s->fillRect);
	track("Set Slider Fill");
	EntityDropField(scene, "Handle rect", s->handleRect);
	track("Set Slider Handle");
	EventFields(scene, "On value changed", s->onValueChanged, s->eventTarget, track);
	ImGui::TextDisabled("func %s(GameObject sender, float value)", s->onValueChanged.empty() ? "<name>" : s->onValueChanged.c_str());
}

void DrawUILayout(Scene& scene, Entity e, st::EditorHistory* history)
{
	st::ui::UILayoutComponent* l = st::ui::Layouts(scene).GetComponent(e);
	if (!l) return;
	if (!ComponentHeader(scene, e, st::ui::kLayouts, "UI Layout", true, history)) return;
	const Edits track{ scene, e, history };

	static const char* types[] = { "Horizontal", "Vertical", "Grid" };
	EnumCombo("Type", l->type, types, 3);
	track("Set Layout Type");
	ImGui::DragFloat4("Padding L T R B", &l->padding.x, 0.5f, 0.0f, 4096.0f, "%.0f");
	track("Set Layout Padding");
	ImGui::DragFloat2("Spacing", &l->spacing.x, 0.5f, 0.0f, 4096.0f, "%.0f");
	track("Set Layout Spacing");
	static const char* align[] = { "Upper left", "Upper centre", "Upper right", "Middle left", "Middle centre",
		"Middle right", "Lower left", "Lower centre", "Lower right" };
	int a = (int)std::min(l->childAlignment, 8u);
	if (ImGui::Combo("Child alignment", &a, align, 9)) l->childAlignment = (uint32_t)a;
	track("Set Layout Alignment");
	if (l->type == st::ui::UILayoutComponent::Type::Grid)
	{
		ImGui::DragFloat2("Cell size", &l->cellSize.x, 0.5f, 1.0f, 4096.0f, "%.0f");
		track("Set Grid Cell");
		ImGui::DragInt("Columns (0 = fit)", &l->columns, 0.1f, 0, 256);
		track("Set Grid Columns");
	}
	else
	{
		BitCheckbox("Expand width", l->_flags, st::ui::UILayoutComponent::EXPAND_WIDTH);
		track("Set Layout Expand");
		ImGui::SameLine();
		BitCheckbox("Expand height", l->_flags, st::ui::UILayoutComponent::EXPAND_HEIGHT);
		track("Set Layout Expand");
	}
	BitCheckbox("Fit width", l->_flags, st::ui::UILayoutComponent::FIT_WIDTH);
	track("Set Layout Fit");
	ImGui::SameLine();
	BitCheckbox("Fit height", l->_flags, st::ui::UILayoutComponent::FIT_HEIGHT);
	track("Set Layout Fit");
	ImGui::SameLine();
	BitCheckbox("Reverse", l->_flags, st::ui::UILayoutComponent::REVERSE);
	track("Set Layout Reverse");
}

void DrawUIDocument(Scene& scene, Entity e, st::EditorHistory* history)
{
	st::ui::UIDocumentComponent* d = st::ui::Documents(scene).GetComponent(e);
	if (!d) return;
	if (!ComponentHeader(scene, e, st::ui::kDocuments, "UI Document (RmlUi)", true, history)) return;
	const Edits track{ scene, e, history };

	AssetDropField("Source (.rml)", d->source);
	track("Set Document Source");
	EditString("Fonts", d->fonts);
	track("Set Document Fonts");
	HelpMarker("Extra .ttf/.otf files the page uses, ';' separated. The engine font is always there as "
		"font-family LiberationSans. Use dp units to follow the canvas scale.");
	BitCheckbox("Interactive", d->_flags, st::ui::UIDocumentComponent::INTERACTIVE);
	track("Set Document Interactive");
	EntityDropField(scene, "Event target", d->eventTarget);
	track("Set Document Event Target");
	HelpMarker("onclick=\"Name\" (or any on<event>) in the page calls func Name(GameObject sender, str element) "
		"on the event target's stScripts.");
	ImGui::TextDisabled("%s", st::ui::RmlHost::Get().Status(e).c_str());
	if (ImGui::SmallButton("Reload all documents"))
		st::ui::RmlHost::Get().ReloadAll();
}
