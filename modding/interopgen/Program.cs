// Runs the Il2CppInterop generation step of BepInEx's Il2CppInteropManager on dummy assemblies written by
// dummygen, then writes the assembly-hash.txt BepInEx uses to decide whether the interop folder is current.
// The two steps are separate programs because Cpp2IL and Il2CppInterop need different AsmResolver versions.
// Usage: interopgen <dummyDir> <GameAssembly.dll> <outputDir> [unityLibsDir]
using System.Security.Cryptography;
using System.Text;
using AsmResolver.DotNet;
using Il2CppInterop.Common;
using Il2CppInterop.Generator;
using Il2CppInterop.Generator.Runners;
using Microsoft.Extensions.Logging;

var dummyDir = args[0];
var gameAssembly = args[1];
var output = args[2];
var unityLibs = args.Length > 3 ? args[3] : null;

var sources = Directory.EnumerateFiles(dummyDir, "*.dll").Select(f => AssemblyDefinition.FromFile(f)).ToList();
Console.WriteLine($"Loaded {sources.Count} dummy assemblies");
Directory.CreateDirectory(output);
foreach (var f in Directory.EnumerateFiles(output, "*.dll")) File.Delete(f);

var opts = new GeneratorOptions
{
    GameAssemblyPath = gameAssembly, // BepInEx's ScanMethodRefs default
    Source = sources,
    OutputDir = output,
    UnityBaseLibsDir = unityLibs,
};
using var factory = LoggerFactory.Create(b => b.AddSimpleConsole(o => o.SingleLine = true).SetMinimumLevel(LogLevel.Information));
Il2CppInteropGenerator.Create(opts).AddLogger(factory.CreateLogger("Il2CppInteropGen")).AddInteropAssemblyGenerator().Run();
Console.WriteLine($"Wrote {Directory.EnumerateFiles(output, "*.dll").Count()} interop assemblies to {output}");

// Same inputs and order as Il2CppInteropManager.ComputeHash. The unity-libs folder is only hashed when it is
// shipped next to the interop folder, which we do not do. Cpp2IL's assembly version is 2022.1.0.0 for both the
// bundled and the newer build.
using var md5 = MD5.Create();
void HashBytes(byte[] bytes) => md5.TransformBlock(bytes, 0, bytes.Length, bytes, 0);
HashBytes(File.ReadAllBytes(gameAssembly));
HashBytes(Encoding.UTF8.GetBytes(typeof(InteropAssemblyGenerator).Assembly.GetName().Version!.ToString()));
HashBytes(Encoding.UTF8.GetBytes("2022.1.0.0"));
md5.TransformFinalBlock(Array.Empty<byte>(), 0, 0);
var hash = Convert.ToHexString(md5.Hash!).ToLowerInvariant();
File.WriteAllText(Path.Combine(output, "assembly-hash.txt"), hash);
Console.WriteLine($"assembly-hash.txt: {hash}");
