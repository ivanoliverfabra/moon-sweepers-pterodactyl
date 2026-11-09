// Reads what a BepInEx mod DLL says about itself, without loading it: the [BepInPlugin] (guid, name, version) of each
// plugin class, its [BepInDependency] list, preloader patchers ([PatcherPluginInfo]), and the assembly's own title,
// description, author and version. Prints one JSON object per file, in order, as a JSON array.
// Usage: modinfo <file.dll>...   Files that aren't .NET assemblies come back as {"path": ..., "error": ...}.
using System.Collections.Immutable;
using System.Reflection.Metadata;
using System.Reflection.PortableExecutable;
using System.Text.Json;
using System.Text.Json.Nodes;

var results = new JsonArray();
foreach (var path in args)
{
    try
    {
        results.Add(Read(path));
    }
    catch (Exception error)
    {
        results.Add(new JsonObject { ["path"] = path, ["error"] = error.Message });
    }
}
Console.Out.Write(results.ToJsonString(new JsonSerializerOptions { WriteIndented = false }));
return 0;

static JsonObject Read(string path)
{
    using var stream = File.OpenRead(path);
    using var pe = new PEReader(stream);
    if (!pe.HasMetadata) throw new InvalidDataException("not a .NET assembly");
    var md = pe.GetMetadataReader();
    var provider = new ValueProvider();
    var result = new JsonObject { ["path"] = path };

    if (md.IsAssembly)
    {
        var assembly = md.GetAssemblyDefinition();
        result["assembly"] = md.GetString(assembly.Name);
        result["assembly_version"] = assembly.Version.ToString();
        foreach (var handle in assembly.GetCustomAttributes())
        {
            var (type, value) = Decode(md, handle, provider);
            var text = value?.FixedArguments.FirstOrDefault().Value as string;
            if (string.IsNullOrWhiteSpace(text)) continue;
            switch (type)
            {
                case "System.Reflection.AssemblyTitleAttribute": result["title"] = text; break;
                case "System.Reflection.AssemblyDescriptionAttribute": result["description"] = text; break;
                case "System.Reflection.AssemblyCompanyAttribute": result["company"] = text; break;
                case "System.Reflection.AssemblyInformationalVersionAttribute": result["informational_version"] = text; break;
            }
        }
    }

    var plugins = new JsonArray();
    foreach (var typeHandle in md.TypeDefinitions)
    {
        var type = md.GetTypeDefinition(typeHandle);
        JsonObject? plugin = null;
        var dependencies = new JsonArray();
        foreach (var handle in type.GetCustomAttributes())
        {
            var (name, value) = Decode(md, handle, provider);
            if (value is null) continue;
            var fixedArgs = value.Value.FixedArguments;
            switch (name)
            {
                case "BepInEx.BepInPlugin" when fixedArgs.Length >= 3:
                    plugin = new JsonObject
                    {
                        ["kind"] = "plugin",
                        ["guid"] = fixedArgs[0].Value as string,
                        ["name"] = fixedArgs[1].Value as string,
                        ["version"] = fixedArgs[2].Value as string,
                    };
                    break;
                case "BepInEx.Preloader.Core.Patching.PatcherPluginInfoAttribute" when fixedArgs.Length >= 3:
                    plugin = new JsonObject
                    {
                        ["kind"] = "patcher",
                        ["guid"] = fixedArgs[0].Value as string,
                        ["name"] = fixedArgs[1].Value as string,
                        ["version"] = fixedArgs[2].Value as string,
                    };
                    break;
                case "BepInEx.BepInDependency" when fixedArgs.Length >= 1:
                    var dependency = new JsonObject { ["guid"] = fixedArgs[0].Value as string };
                    if (fixedArgs.Length >= 2 && fixedArgs[1].Value is string range) dependency["version"] = range;
                    // DependencyFlags.SoftDependency = 2; HardDependency = 1 (the default).
                    dependency["soft"] = fixedArgs.Length >= 2 && fixedArgs[1].Value is int flags && (flags & 2) != 0;
                    dependencies.Add(dependency);
                    break;
            }
        }
        if (plugin is null) continue;
        plugin["class"] = md.GetString(type.Namespace) is { Length: > 0 } ns
            ? $"{ns}.{md.GetString(type.Name)}"
            : md.GetString(type.Name);
        if (dependencies.Count > 0) plugin["dependencies"] = dependencies;
        plugins.Add(plugin);
    }
    result["plugins"] = plugins;
    return result;
}

static (string? Type, CustomAttributeValue<object?>? Value) Decode(
    MetadataReader md, CustomAttributeHandle handle, ValueProvider provider)
{
    var attribute = md.GetCustomAttribute(handle);
    string? type = attribute.Constructor.Kind switch
    {
        HandleKind.MemberReference => TypeName(md, md.GetMemberReference((MemberReferenceHandle)attribute.Constructor).Parent),
        HandleKind.MethodDefinition => TypeName(md, md.GetMethodDefinition((MethodDefinitionHandle)attribute.Constructor).GetDeclaringType()),
        _ => null,
    };
    if (type is null) return (null, null);
    try
    {
        return (type, attribute.DecodeValue(provider));
    }
    catch (Exception)
    {
        return (type, null); // an argument type we don't understand; skip the attribute
    }
}

static string? TypeName(MetadataReader md, EntityHandle handle)
{
    switch (handle.Kind)
    {
        case HandleKind.TypeReference:
            var reference = md.GetTypeReference((TypeReferenceHandle)handle);
            return $"{md.GetString(reference.Namespace)}.{md.GetString(reference.Name)}";
        case HandleKind.TypeDefinition:
            var definition = md.GetTypeDefinition((TypeDefinitionHandle)handle);
            return $"{md.GetString(definition.Namespace)}.{md.GetString(definition.Name)}";
        default:
            return null;
    }
}

/// <summary>Turns attribute argument types into plain values; enums (like DependencyFlags) are read as int.</summary>
sealed class ValueProvider : ICustomAttributeTypeProvider<object?>
{
    public object? GetPrimitiveType(PrimitiveTypeCode typeCode) => typeCode;
    public object? GetSystemType() => typeof(Type);
    public object? GetSZArrayType(object? elementType) => null;
    public object? GetTypeFromDefinition(MetadataReader reader, TypeDefinitionHandle handle, byte rawTypeKind) => PrimitiveTypeCode.Int32;
    public object? GetTypeFromReference(MetadataReader reader, TypeReferenceHandle handle, byte rawTypeKind) => PrimitiveTypeCode.Int32;
    public object? GetTypeFromSerializedName(string name) => PrimitiveTypeCode.Int32;
    public PrimitiveTypeCode GetUnderlyingEnumType(object? type) => PrimitiveTypeCode.Int32;
    public bool IsSystemType(object? type) => type is Type;
}
