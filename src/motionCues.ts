import {call} from "@decky/api"

export interface MotionCueSettings {
    enabled: boolean
    dotColor: string
    dotAmount: number
    dotSize: number
    transparency: number
    response: number
    waveMotion: boolean
    simulateMotion: boolean
}

export const defaultSettings: MotionCueSettings = {
    enabled: false,
    dotColor: "#ffffff",
    dotAmount: 4,
    dotSize: 1,
    transparency: 0.3,
    response: 1,
    waveMotion: false,
    simulateMotion: false,
}

// --- Color helpers (Decky's ColorPickerModal speaks hsla, the renderer wants hex) ---

const clamp01 = (v: number) => (v < 0 ? 0 : v > 1 ? 1 : v)

export const hexToHsl = (hex: string): {h: number; s: number; l: number} => {
    const clean = hex.replace("#", "")
    const r = parseInt(clean.slice(0, 2), 16) / 255
    const g = parseInt(clean.slice(2, 4), 16) / 255
    const b = parseInt(clean.slice(4, 6), 16) / 255
    const max = Math.max(r, g, b)
    const min = Math.min(r, g, b)
    const l = (max + min) / 2
    let h = 0
    let s = 0
    if (max !== min) {
        const d = max - min
        s = l > 0.5 ? d / (2 - max - min) : d / (max + min)
        if (max === r) h = (g - b) / d + (g < b ? 6 : 0)
        else if (max === g) h = (b - r) / d + 2
        else h = (r - g) / d + 4
        h /= 6
    }
    return {h: Math.round(h * 360), s: Math.round(s * 100), l: Math.round(l * 100)}
}

const hslChannel = (h: number, s: number, l: number, n: number): number => {
    const k = (n + h / 30) % 12
    const a = s * Math.min(l, 1 - l)
    const color = l - a * Math.max(-1, Math.min(k - 3, 9 - k, 1))
    return Math.round(clamp01(color) * 255)
}

const toHex2 = (v: number) => v.toString(16).padStart(2, "0")

export const hslToHex = (h: number, s: number, l: number): string => {
    const sn = s / 100
    const ln = l / 100
    return `#${toHex2(hslChannel(h, sn, ln, 0))}${toHex2(hslChannel(h, sn, ln, 8))}${toHex2(hslChannel(h, sn, ln, 4))}`
}

// Parses "hsla(H, S%, L%, A)" (the value ColorPickerModal returns) into a #rrggbb hex string.
export const hslaToHex = (hsla: string): string => {
    const match = hsla.match(/hsla?\(\s*([\d.]+)\s*,\s*([\d.]+)%\s*,\s*([\d.]+)%/i)
    if (!match)
        return defaultSettings.dotColor
    return hslToHex(Number(match[1]), Number(match[2]), Number(match[3]))
}

export interface PluginCallSuccess<T = unknown> {
    success: true
    result: T
}

export interface PluginCallError {
    success: false
    result: string
}

export type PluginCallResult<T = unknown> = PluginCallSuccess<T> | PluginCallError

interface PluginMethodCaller {
    <Args extends Record<string, unknown> = Record<string, never>, Return = unknown>(
        route: string,
        args?: Args
    ): Promise<PluginCallResult<Return>>
}

export interface PluginAPI {
    callPluginMethod: PluginMethodCaller
}

const callPluginMethod: PluginMethodCaller = async <Args extends Record<string, unknown>, Return>(
    route: string,
    args?: Args
) => {
    try {
        const values = Object.values(args ?? {})
        const result = await call<unknown[], Return | PluginCallResult<Return>>(route, ...values)
        if (
            result &&
            typeof result === "object" &&
            "success" in result &&
            typeof result.success === "boolean" &&
            "result" in result
        )
            return result as PluginCallResult<Return>
        return {success: true, result: result as Return}
    } catch (error) {
        return {
            success: false,
            result: `${route}: ${error instanceof Error ? error.message : String(error)}`,
        }
    }
}

export const pluginAPI: PluginAPI = {
    callPluginMethod,
}
