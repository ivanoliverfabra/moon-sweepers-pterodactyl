// Writes the Cpp2IL "dummy" assemblies BepInEx would generate on first start, using a Cpp2IL new enough for
// metadata v108. Mirrors BepInEx.Unity.IL2CPP's Il2CppInteropManager.RunCpp2Il.
// Usage: dummygen <GameAssembly.dll> <global-metadata.dat> <unity version, e.g. 6000.6.0f1> <outputDir>
using AssetRipper.Primitives;
using Cpp2IL.Core;
using Cpp2IL.Core.Api;
using Cpp2IL.Core.OutputFormats;
using Cpp2IL.Core.ProcessingLayers;
using LibCpp2IL;

var (gameAssembly, metadata, version, output) = (args[0], args[1], args[2], args[3]);
Cpp2IL.Core.Logging.Logger.InfoLog += (message, source) => Console.WriteLine($"[{source}] {message.Trim()}");
Cpp2IL.Core.Logging.Logger.WarningLog += (message, source) => Console.WriteLine($"[{source}] warning: {message.Trim()}");
Cpp2IL.Core.Logging.Logger.ErrorLog += (message, source) => Console.Error.WriteLine($"[{source}] error: {message.Trim()}");

Cpp2IlApi.Init();
Cpp2IlApi.InitializeLibCpp2Il(gameAssembly, metadata, UnityVersion.Parse(version), false);
List<Cpp2IlProcessingLayer> layers = new() { new AttributeInjectorProcessingLayer() };
foreach (var layer in layers) layer.PreProcess(Cpp2IlApi.CurrentAppContext!, layers);
foreach (var layer in layers) layer.Process(Cpp2IlApi.CurrentAppContext!);
var assemblies = new AsmResolverDllOutputFormatDefault().BuildAssemblies(Cpp2IlApi.CurrentAppContext!);

Directory.CreateDirectory(output);
foreach (var f in Directory.EnumerateFiles(output, "*.dll")) File.Delete(f);
foreach (var assembly in assemblies) assembly.Write(Path.Combine(output, $"{assembly.Name}.dll"));
LibCpp2IlMain.Reset();
Console.WriteLine($"Wrote {assemblies.Count} dummy assemblies to {output}");
