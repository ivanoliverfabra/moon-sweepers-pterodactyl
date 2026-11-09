using System.Collections;
using System.Linq;
using BepInEx;
using BepInEx.Logging;
using BepInEx.Unity.IL2CPP;
using BepInEx.Unity.IL2CPP.Utils.Collections;
using HarmonyLib;
using UnityEngine;

namespace MoonModTest;

[BepInPlugin("dev.moonsweepers.modtest", "Moon mod loader test", "0.1.0")]
public class Plugin : BasePlugin
{
    internal static ManualLogSource Log;

    public override void Load()
    {
        Log = base.Log;
        Log.LogInfo("Load() ran");
        // Class injection: a MonoBehaviour subclass, created and updated by Unity.
        AddComponent<Probe>();
        // Harmony on game code.
        var harmony = new Harmony("dev.moonsweepers.modtest");
        harmony.PatchAll(typeof(Patches));
        Log.LogInfo($"Patched {harmony.GetPatchedMethods().Count()} game method(s)");
    }
}

public class Probe : MonoBehaviour
{
    private int frames;

    private void Start()
    {
        Plugin.Log.LogInfo("Probe.Start ran (injected MonoBehaviour is alive)");
        // Interface injection: Unity drives this managed IEnumerator through Il2CppSystem.Collections.IEnumerator.
        StartCoroutine(Countdown().WrapToIl2Cpp());
    }

    private void Update()
    {
        if (++frames == 60) Plugin.Log.LogInfo("Probe.Update reached 60 frames");
    }

    private IEnumerator Countdown()
    {
        for (var i = 3; i > 0; i--)
        {
            Plugin.Log.LogInfo($"Coroutine step {i}");
            yield return null;
        }
        Plugin.Log.LogInfo("Coroutine finished (interface dispatch works)");
    }
}

[HarmonyPatch]
internal static class Patches
{
    private static bool said;

    [HarmonyPatch(typeof(MoonCleaners.MoonSteam), "Update")]
    [HarmonyPrefix]
    private static void UpdatePrefix(MoonCleaners.MoonSteam __instance)
    {
        if (said) return;
        said = true;
        Plugin.Log.LogInfo($"Harmony prefix on MoonSteam.Update ran (on {__instance.name})");
    }
}
