#!/usr/bin/env node

const fs = require("fs");
const path = require("path");
const JSZip = require("jszip");

const extensionRoot = path.resolve(__dirname, "..");

function xmlEscape(value)
{
    return String(value ?? "")
        .replace(/&/g, "&amp;")
        .replace(/</g, "&lt;")
        .replace(/>/g, "&gt;")
        .replace(/"/g, "&quot;");
}

function contentTypesXml()
{
    return [
        '<?xml version="1.0" encoding="utf-8"?>',
        '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">',
        '<Default Extension=".json" ContentType="application/json"/>',
        '<Default Extension=".vsixmanifest" ContentType="text/xml"/>',
        '<Default Extension=".js" ContentType="application/javascript"/>',
        '<Default Extension=".md" ContentType="text/markdown"/>',
        '<Default Extension=".html" ContentType="text/html"/>',
        '</Types>'
    ].join("");
}

function tagValues(pkg)
{
    const tags = new Set(["atomicproof-contract"]);
    for (const category of pkg.categories || []) {
        tags.add(category.toLowerCase());
    }
    for (const language of pkg.contributes?.languages || []) {
        for (const alias of language.aliases || []) {
            tags.add(alias);
        }
        for (const extension of language.extensions || []) {
            tags.add("__ext_" + String(extension).replace(/^\./, ""));
        }
    }
    return Array.from(tags).join(",");
}

function vsixManifest(pkg)
{
    const categories = Array.isArray(pkg.categories) ? pkg.categories.join(",") : "";
    return [
        '<?xml version="1.0" encoding="utf-8"?>',
        '<PackageManifest Version="2.0.0" ',
        'xmlns="http://schemas.microsoft.com/developer/vsx-schema/2011" ',
        'xmlns:d="http://schemas.microsoft.com/developer/vsx-schema-design/2011">',
        '<Metadata>',
        '<Identity Language="en-US" ',
        `Id="${xmlEscape(pkg.name)}" `,
        `Version="${xmlEscape(pkg.version)}" `,
        `Publisher="${xmlEscape(pkg.publisher)}" />`,
        `<DisplayName>${xmlEscape(pkg.displayName || pkg.name)}</DisplayName>`,
        `<Description xml:space="preserve">${xmlEscape(pkg.description || "")}</Description>`,
        `<Tags>${xmlEscape(tagValues(pkg))}</Tags>`,
        `<Categories>${xmlEscape(categories)}</Categories>`,
        '<GalleryFlags>Public</GalleryFlags>',
        '<Properties>',
        `<Property Id="Microsoft.VisualStudio.Code.Engine" Value="${xmlEscape(pkg.engines?.vscode || "*")}" />`,
        '<Property Id="Microsoft.VisualStudio.Code.ExtensionDependencies" Value="" />',
        '<Property Id="Microsoft.VisualStudio.Code.ExtensionPack" Value="" />',
        '<Property Id="Microsoft.VisualStudio.Code.ExtensionKind" Value="workspace" />',
        '<Property Id="Microsoft.VisualStudio.Code.LocalizedLanguages" Value="" />',
        '<Property Id="Microsoft.VisualStudio.Services.GitHubFlavoredMarkdown" Value="true" />',
        '<Property Id="Microsoft.VisualStudio.Services.Content.Pricing" Value="Free"/>',
        '</Properties>',
        '</Metadata>',
        '<Installation><InstallationTarget Id="Microsoft.VisualStudio.Code"/></Installation>',
        '<Dependencies/>',
        '<Assets>',
        '<Asset Type="Microsoft.VisualStudio.Code.Manifest" Path="extension/package.json" Addressable="true" />',
        '<Asset Type="Microsoft.VisualStudio.Services.Content.Details" Path="extension/README.md" Addressable="true" />',
        '</Assets>',
        '</PackageManifest>'
    ].join("");
}

function parseOutArg(argv)
{
    const outIndex = argv.indexOf("--out");
    if (outIndex !== -1 && argv[outIndex + 1]) {
        return path.resolve(process.cwd(), argv[outIndex + 1]);
    }
    const pkg = JSON.parse(fs.readFileSync(path.join(extensionRoot, "package.json"), "utf8"));
    return path.join(extensionRoot, `${pkg.name}-${pkg.version}.vsix`);
}

async function main()
{
    const pkg = JSON.parse(fs.readFileSync(path.join(extensionRoot, "package.json"), "utf8"));
    const outFile = parseOutArg(process.argv.slice(2));
    const files = [
        "README.md",
        "extension.js",
        "language-configuration.json",
        "package.json",
        "schemas/apc-stack-trace.schema.json",
        "stack_visualizer/index.html"
    ];

    const zip = new JSZip();
    zip.file("[Content_Types].xml", contentTypesXml());
    zip.file("extension.vsixmanifest", vsixManifest(pkg));

    for (const relativePath of files) {
        const sourcePath = path.join(extensionRoot, relativePath);
        if (!fs.existsSync(sourcePath)) {
            throw new Error(`Package input is missing: ${relativePath}`);
        }
        zip.file(
            "extension/" + relativePath.replace(/\\/g, "/"),
            fs.readFileSync(sourcePath)
        );
    }

    fs.mkdirSync(path.dirname(outFile), { recursive: true });
    const data = await zip.generateAsync({
        type: "nodebuffer",
        compression: "DEFLATE"
    });
    fs.writeFileSync(outFile, data);
    console.log(`Created ${path.relative(process.cwd(), outFile)} (${data.length} bytes)`);
}

main().catch((error) => {
    console.error(error.stack || error.message);
    process.exit(1);
});
