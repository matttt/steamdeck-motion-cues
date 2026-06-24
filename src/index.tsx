import {
    ColorPickerModal,
    DialogButton,
    Focusable,
    PanelSection,
    PanelSectionRow,
    showModal,
    SliderField,
    staticClasses,
    ToggleField,
} from "@decky/ui"
import {definePlugin} from "@decky/api"
import {FC, useEffect, useState} from "react"
import {FaBraille, FaCog} from "react-icons/fa"
import {defaultSettings, hexToHsl, hslaToHex, MotionCueSettings, PluginAPI, pluginAPI} from "./motionCues"

declare global {
    interface Window {
        NotificationStore: any
    }
}

// Mirrors the toggle state so the background loop can read it without React.
let motionCuesEnabled = false

const colorPresets = ["#ffffff", "#00d9ff", "#73ff6b", "#ffcf4a", "#ff4f81"]

const GEAR_KEY = "__gear__"

const Content: FC<{serverAPI: PluginAPI}> = ({serverAPI}) => {
    const [settings, setSettings] = useState<MotionCueSettings>({...defaultSettings})
    const [hovered, setHovered] = useState<string | null>(null)

    const apply = (next: MotionCueSettings) => {
        setSettings(next)
        motionCuesEnabled = next.enabled
    }

    const reloadSettings = async () => {
        const result = await serverAPI.callPluginMethod<Record<string, never>, MotionCueSettings>("get_settings", {})
        if (result.success)
            apply(result.result)
        else
            console.error("Motion Cue Dots: failed to load settings", result.result)
    }

    useEffect(() => {
        reloadSettings()
    }, [])

    const saveSettings = async (patch: Partial<MotionCueSettings>) => {
        apply({...settings, ...patch})

        const result = await serverAPI.callPluginMethod<{settings: Partial<MotionCueSettings>}, MotionCueSettings>(
            "save_settings",
            {settings: patch}
        )
        if (result.success)
            apply(result.result)
        else
            console.error("Motion Cue Dots: failed to save settings", result.result)
    }

    const openColorPicker = () => {
        const {h, s, l} = hexToHsl(settings.dotColor)
        showModal(
            <ColorPickerModal
                closeModal={() => {}}
                title="Dot Color"
                defaultH={h}
                defaultS={s}
                defaultL={l}
                defaultA={1}
                onConfirm={(hsla: string) => saveSettings({dotColor: hslaToHex(hsla)})}
            />
        )
    }

    const swatchSize = "30px"

    // Black ring just inside the edge + white ring around the circumference on hover/focus.
    const ringShadow = (key: string, color: string) =>
        hovered === key
            ? "inset 0 0 0 2px #000000, 0 0 0 3px #ffffff"
            : settings.dotColor.toLowerCase() === color
                ? "0 0 0 2px #ffffff"
                : "inset 0 0 0 1px rgba(255,255,255,0.35)"

    // DialogButton forwards Decky's gamepad focus callbacks reliably; plain DOM
    // onFocus/onBlur are not part of its props and fire inconsistently under
    // gamepad navigation. Use the gamepad callbacks (plus mouse for desktop).
    const clearIfCurrent = (key: string) => setHovered((current) => (current === key ? null : current))
    const hoverHandlers = (key: string) => ({
        onGamepadFocus: () => setHovered(key),
        onGamepadBlur: () => clearIfCurrent(key),
        onMouseEnter: () => setHovered(key),
        onMouseLeave: () => clearIfCurrent(key),
    })

    const isCustomColor = !colorPresets.includes(settings.dotColor.toLowerCase())

    return (
        <PanelSection title="Motion Cue Dots">
            <PanelSectionRow>
                <ToggleField
                    checked={settings.enabled}
                    label="Enable"
                    description="For best results, enable after launching a game"
                    onChange={(checked) => saveSettings({enabled: checked})}
                />
            </PanelSectionRow>

            <PanelSectionRow>
                <ToggleField
                    checked={settings.waveMotion}
                    label="Wave Motion"
                    description="Checkerboard dots pulse in size as the field drifts"
                    onChange={(checked) => saveSettings({waveMotion: checked})}
                />
            </PanelSectionRow>

            <PanelSectionRow>
                <div style={{display: "flex", flexDirection: "column", gap: "6px", width: "100%"}}>
                    <div style={{fontSize: "0.9em"}}>Dot Color</div>
                    <Focusable style={{display: "flex", alignItems: "center", gap: "10px"}}>
                        {colorPresets.map((color) => (
                            <DialogButton
                                key={color}
                                onClick={() => saveSettings({dotColor: color})}
                                {...hoverHandlers(color)}
                                style={{
                                    minWidth: 0,
                                    width: swatchSize,
                                    height: swatchSize,
                                    padding: 0,
                                    border: "none",
                                    borderRadius: "50%",
                                    background: color,
                                    boxShadow: ringShadow(color, color),
                                }}
                            />
                        ))}
                        <DialogButton
                            onClick={openColorPicker}
                            {...hoverHandlers(GEAR_KEY)}
                            style={{
                                minWidth: 0,
                                width: swatchSize,
                                height: swatchSize,
                                padding: 0,
                                marginLeft: "auto",
                                display: "flex",
                                alignItems: "center",
                                justifyContent: "center",
                                border: "none",
                                borderRadius: "50%",
                                background: isCustomColor ? settings.dotColor : "rgba(255,255,255,0.08)",
                                boxShadow:
                                    hovered === GEAR_KEY
                                        ? "inset 0 0 0 2px #000000, 0 0 0 3px #ffffff"
                                        : isCustomColor
                                            ? "0 0 0 2px #ffffff"
                                            : "inset 0 0 0 1px rgba(255,255,255,0.35)",
                            }}
                        >
                            <FaCog/>
                        </DialogButton>
                    </Focusable>
                </div>
            </PanelSectionRow>

            <PanelSectionRow>
                <SliderField
                    label="Dot Amount"
                    value={settings.dotAmount}
                    min={2}
                    max={24}
                    step={1}
                    showValue
                    notchTicksVisible
                    onChange={(value) => saveSettings({dotAmount: Math.round(value)})}
                />
            </PanelSectionRow>

            <PanelSectionRow>
                <SliderField
                    label="Dot Size"
                    value={settings.dotSize}
                    min={0.25}
                    max={3}
                    step={0.05}
                    showValue
                    onChange={(value) => saveSettings({dotSize: value})}
                />
            </PanelSectionRow>

            <PanelSectionRow>
                <SliderField
                    label="Transparency"
                    value={settings.transparency}
                    min={0.05}
                    max={1}
                    step={0.05}
                    showValue
                    onChange={(value) => saveSettings({transparency: value})}
                />
            </PanelSectionRow>

            <PanelSectionRow>
                <SliderField
                    label="Motion Response"
                    value={settings.response}
                    min={0.25}
                    max={3}
                    step={0.25}
                    showValue
                    onChange={(value) => saveSettings({response: value})}
                />
            </PanelSectionRow>
        </PanelSection>
    )
}

// Seed the enabled mirror on load so the loop works even before the panel is opened.
const loadSettingsIntoMirror = async () => {
    const result = await pluginAPI.callPluginMethod<Record<string, never>, MotionCueSettings>("get_settings", {})
    if (result.success)
        motionCuesEnabled = result.result.enabled
}

export default definePlugin(() => {
    loadSettingsIntoMirror()

    let shown = false
    const timer = setInterval(() => {
        const isInGame = window.NotificationStore?.BIsUserInGame?.() ?? false
        const shouldShow = motionCuesEnabled && isInGame
        if (shouldShow !== shown) {
            pluginAPI.callPluginMethod(shouldShow ? "create" : "destroy", {})
            shown = shouldShow
        }
    }, 100)

    return {
        name: "Motion Cue Dots",
        titleView: <div className={staticClasses.Title}>Motion Cues</div>,
        content: <Content serverAPI={pluginAPI}/>,
        icon: <FaBraille/>,
        alwaysRender: true,
        onDismount() {
            clearInterval(timer)
            pluginAPI.callPluginMethod("destroy", {})
        },
    }
})
