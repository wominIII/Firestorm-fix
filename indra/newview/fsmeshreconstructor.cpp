/**
 * @file fsmeshreconstructor.cpp
 * @brief DAE writer for mesh data already decoded by LLMeshRepository.
 */

#include "llviewerprecompiledheaders.h"

#include "fsmeshreconstructor.h"
#include "fsexportperms.h"

#include "llfilepicker.h"
#include "llfile.h"
#include "llimagepng.h"
#include "lljoint.h"
#include "llmaterial.h"
#include "llkeyframemotion.h"
#include "llmodel.h"
#include "llnotificationsutil.h"
#include "llprimitive.h"
#include "llselectmgr.h"
#include "llviewerobjectlist.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "llviewermenufile.h"
#include "llvovolume.h"
#include "llvoavatarself.h"
#include "llvolume.h"
#include "llvolumemgr.h"

#include <cmath>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <openssl/evp.h>
#include <openssl/pem.h>

namespace
{
constexpr S32 FIRST_MESH_LOD = LLModel::LOD_IMPOSTOR;
constexpr S32 LAST_MESH_LOD = LLModel::LOD_HIGH;

const char* MESH_EXPORT_PUBLIC_KEY =
    "-----BEGIN PUBLIC KEY-----\n"
    "MIIBojANBgkqhkiG9w0BAQEFAAOCAY8AMIIBigKCAYEAvUrTj1CYz+8zrKqTypaM\n"
    "jfxqUTtsaIb0aFO6U1pLM92MynQukV9ZU9/NLC6yxfl3AiJOh5hRocoD4u87llGA\n"
    "LgVWQn/z2RWelYwadfR0M9z/rQwAold2DbiOoL8bf9CZ1D90AEABw/GpxkwIGHOt\n"
    "6wSWwgmct7zZZIuXJsC781v50vo+h+EEYExcM5tAfhdqpB3Gn6YHkGAqcMCC3y79\n"
    "xq0Z+kwoXkAS1JQZBRWWuvrYHRU2tVXwlW6V6WRrD3mlYKIrivJ8qQVZELGSOB7M\n"
    "o9W17RSQd28k8wOwXt3c4FKJPq7Cb0na4d5M3SFtOdBtsIheioGGcQeShfy0houl\n"
    "KePZSBhqEjtV9DjqwtmhMkQMoFsREJyZiRQmkYmo6nKK0gjeDvcEutyIOk4inPBZ\n"
    "293fNWlCEkNqeL2rfvDE/iDevyr1j1B+l4dO6WU0FHftiofh61/fepHefFFeQoNR\n"
    "cAa1Lxd1bnatU1oNci2UjBcWtA1LWflLoUV0b7jZ07j5AgMBAAE=\n"
    "-----END PUBLIC KEY-----\n";

bool verify_mesh_export_license()
{
    const std::string filename = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "mesh_export.license");
    llifstream input(filename.c_str(), std::ios::in | std::ios::binary);
    if (!input.is_open()) return false;

    std::string version, feature, machine, signature_line;
    std::getline(input, version);
    std::getline(input, feature);
    std::getline(input, machine);
    std::getline(input, signature_line);
    if (version != "version=1" || feature != "feature=mesh_export" ||
        machine.rfind("machine=", 0) != 0 || signature_line.rfind("signature=", 0) != 0)
        return false;

    // A signed portable license may be copied to another computer.  The
    // wildcard is still protected by the signature, so an unsigned or edited
    // file cannot enable the feature.
    if (machine != "machine=*")
        return false;

    const std::string payload = version + "\n" + feature + "\n" + machine + "\n";
    const std::string encoded = signature_line.substr(10);
    std::vector<unsigned char> signature((encoded.size() * 3) / 4 + 3);
    int signature_size = EVP_DecodeBlock(signature.data(),
        reinterpret_cast<const unsigned char*>(encoded.data()), static_cast<int>(encoded.size()));
    if (signature_size <= 0) return false;
    if (!encoded.empty() && encoded.back() == '=') --signature_size;
    if (encoded.size() > 1 && encoded[encoded.size() - 2] == '=') --signature_size;

    BIO* bio = BIO_new_mem_buf(MESH_EXPORT_PUBLIC_KEY, -1);
    EVP_PKEY* key = bio ? PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr) : nullptr;
    EVP_MD_CTX* context = key ? EVP_MD_CTX_new() : nullptr;
    bool valid = false;
    if (context && EVP_DigestVerifyInit(context, nullptr, EVP_sha256(), nullptr, key) == 1 &&
        EVP_DigestVerifyUpdate(context, payload.data(), payload.size()) == 1)
    {
        valid = EVP_DigestVerifyFinal(context, signature.data(), signature_size) == 1;
    }
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    BIO_free(bio);
    return valid;
}

const char* lod_name(S32 lod)
{
    static const char* const names[] = { "lowest", "low", "medium", "high" };
    return lod >= FIRST_MESH_LOD && lod <= LAST_MESH_LOD ? names[lod] : "unknown";
}

std::string escape_xml(const std::string& text)
{
    std::string result;
    result.reserve(text.size());
    for (const char c : text)
    {
        switch (c)
        {
        case '&': result += "&amp;"; break;
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        case '\"': result += "&quot;"; break;
        case '\'': result += "&apos;"; break;
        default: result += c; break;
        }
    }
    return result;
}

std::string escape_json(const std::string& text)
{
    std::string result;
    result.reserve(text.size());
    for (const char c : text)
    {
        switch (c)
        {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += c; break;
        }
    }
    return result;
}

void write_float_source(std::ostream& out, const std::string& id, const std::vector<F32>& values, S32 stride, const char* params)
{
    out << "      <source id=\"" << id << "\"><float_array id=\"" << id << "-array\" count=\"" << values.size() << "\">";
    for (F32 value : values) out << value << ' ';
    out << "</float_array><technique_common><accessor source=\"#" << id << "-array\" count=\"" << values.size() / stride
        << "\" stride=\"" << stride << "\">";
    for (S32 i = 0; i < stride; ++i) out << "<param name=\"" << params[i] << "\" type=\"float\"/>";
    out << "</accessor></technique_common></source>\n";
}

void write_matrix(std::ostream& out, const LLMatrix4a& matrix)
{
    const F32* values = matrix.getF32ptr();
    for (S32 column = 0; column < 4; ++column)
    {
        for (S32 row = 0; row < 4; ++row) out << values[row * 4 + column] << ' ';
    }
}

F32 get_skin_unit_meter(const LLMeshSkinInfo& skin)
{
    if (skin.mInvBindMatrix.empty()) return 1.f;

    U32 matrix_index = 0;
    const auto pelvis = std::find(skin.mJointNames.begin(), skin.mJointNames.end(), "mPelvis");
    if (pelvis != skin.mJointNames.end())
    {
        matrix_index = static_cast<U32>(std::distance(skin.mJointNames.begin(), pelvis));
    }
    if (matrix_index >= skin.mInvBindMatrix.size()) return 1.f;

    // The linear part of an inverse bind matrix retains the source DAE unit.
    // A mesh authored in centimetres, for example, has basis lengths near 0.01.
    const F32* matrix = skin.mInvBindMatrix[matrix_index].getF32ptr();
    const F32 basis_x = std::sqrt(matrix[0] * matrix[0] + matrix[1] * matrix[1] + matrix[2] * matrix[2]);
    const F32 basis_y = std::sqrt(matrix[4] * matrix[4] + matrix[5] * matrix[5] + matrix[6] * matrix[6]);
    const F32 basis_z = std::sqrt(matrix[8] * matrix[8] + matrix[9] * matrix[9] + matrix[10] * matrix[10]);
    const F32 unit_meter = (basis_x + basis_y + basis_z) / 3.f;
    return std::isfinite(unit_meter) && unit_meter >= 0.000001f && unit_meter <= 1000.f ? unit_meter : 1.f;
}

bool face_has_render_data(const LLVolumeFace& face)
{
    return face.mNumVertices > 0 && face.mNumIndices >= 3 && face.mPositions && face.mNormals && face.mTexCoords && face.mIndices;
}

LLVOVolume* find_decoded_mesh(LLViewerObject* object)
{
    if (!object) return nullptr;
    if (LLVOVolume* volume = dynamic_cast<LLVOVolume*>(object))
    {
        if (volume->isMesh() && volume->getVolume() && volume->getVolume()->isMeshAssetLoaded())
        {
            return volume;
        }
    }
    for (const LLPointer<LLViewerObject>& child : object->getChildren())
    {
        if (LLVOVolume* mesh = find_decoded_mesh(child.get())) return mesh;
    }
    return nullptr;
}

S32 write_geometry(std::ostream& out, const LLVolume& volume, const LLViewerObject& object, S32 lod)
{
    const std::string id = std::string("viewer-cache-") + lod_name(lod);
    std::vector<F32> positions;
    std::vector<F32> normals;
    std::vector<F32> texcoords;
    for (S32 face_index = 0; face_index < volume.getNumVolumeFaces(); ++face_index)
    {
        const LLVolumeFace& face = volume.getVolumeFace(face_index);
        if (!face_has_render_data(face)) continue;
        for (S32 vertex = 0; vertex < face.mNumVertices; ++vertex)
        {
            const F32* position = face.mPositions[vertex].getF32ptr();
            const F32* normal = face.mNormals[vertex].getF32ptr();
            positions.insert(positions.end(), position, position + 3);
            normals.insert(normals.end(), normal, normal + 3);
            F32 u = face.mTexCoords[vertex].mV[0];
            F32 v = face.mTexCoords[vertex].mV[1];
            if (const LLTextureEntry* te = object.getTE(face_index))
            {
                const F32 centered_u = u - .5f;
                const F32 centered_v = v - .5f;
                const F32 cosine = std::cos(te->getRotation());
                const F32 sine = std::sin(te->getRotation());
                u = (centered_u * cosine + centered_v * sine) * te->getScaleS() + te->getOffsetS() + .5f;
                v = (-centered_u * sine + centered_v * cosine) * te->getScaleT() + te->getOffsetT() + .5f;
            }
            texcoords.push_back(u);
            texcoords.push_back(v);
        }
    }
    if (positions.empty()) return 0;

    out << "    <geometry id=\"" << id << "\" name=\"Viewer cached " << lod_name(lod) << " LOD\"><mesh>\n";
    write_float_source(out, id + "-positions", positions, 3, "XYZ");
    write_float_source(out, id + "-normals", normals, 3, "XYZ");
    write_float_source(out, id + "-map0", texcoords, 2, "ST");
    out << "      <vertices id=\"" << id << "-vertices\"><input semantic=\"POSITION\" source=\"#" << id << "-positions\"/></vertices>\n";

    S32 vertex_offset = 0;
    S32 triangles = 0;
    for (S32 face_index = 0; face_index < volume.getNumVolumeFaces(); ++face_index)
    {
        const LLVolumeFace& face = volume.getVolumeFace(face_index);
        if (!face_has_render_data(face)) continue;
        const S32 face_triangles = face.mNumIndices / 3;
        out << "      <triangles material=\"slot-" << face_index << "\" count=\"" << face_triangles << "\">"
            << "<input semantic=\"VERTEX\" source=\"#" << id << "-vertices\" offset=\"0\"/>"
            << "<input semantic=\"NORMAL\" source=\"#" << id << "-normals\" offset=\"1\"/>"
            << "<input semantic=\"TEXCOORD\" source=\"#" << id << "-map0\" offset=\"2\" set=\"0\"/><p>";
        for (S32 index = 0; index < face.mNumIndices; ++index)
        {
            const U32 vertex = vertex_offset + face.mIndices[index];
            out << vertex << ' ' << vertex << ' ' << vertex << ' ';
        }
        out << "</p></triangles>\n";
        vertex_offset += face.mNumVertices;
        triangles += face_triangles;
    }
    out << "    </mesh></geometry>\n";
    return triangles;
}

struct ExportedTexture
{
    LLUUID id;
    std::string uri;
};

using texture_map_t = std::map<LLUUID, ExportedTexture>;

void export_active_animations(const std::string& dae_filename)
{
    if (!gAgentAvatarp) return;

    const std::string animation_dir = gDirUtilp->add(gDirUtilp->getDirName(dae_filename), "animations");
    if (!LLFile::isdir(animation_dir) && LLFile::mkdir(animation_dir) != 0) return;

    std::ofstream manifest(gDirUtilp->add(animation_dir, "animations.json"),
                           std::ios::out | std::ios::binary | std::ios::trunc);
    if (!manifest.is_open()) return;
    manifest << "{\n  \"source\": \"active motions already decoded by this Viewer session\",\n  \"animations\": [\n";

    bool first = true;
    LLMotionController::motion_list_t& motions = gAgentAvatarp->getMotionController().getActiveMotions();
    for (LLMotion* motion : motions)
    {
        LLKeyframeMotion* keyframe = dynamic_cast<LLKeyframeMotion*>(motion);
        if (!keyframe || !keyframe->isLoaded() || keyframe->getID().isNull()) continue;

        std::string inventory_name;
        std::string inventory_description;
        const bool permitted = FSExportPermsCheck::canExportAsset(keyframe->getID(), &inventory_name, &inventory_description);
        const std::string filename = keyframe->getID().asString() + ".anim";
        const bool exported = permitted && keyframe->dumpToFile(gDirUtilp->add(animation_dir, filename));

        if (!first) manifest << ",\n";
        first = false;
        manifest << "    {\"uuid\": \"" << keyframe->getID() << "\", \"name\": \""
                 << escape_json(inventory_name) << "\", \"duration\": " << keyframe->getDuration()
                 << ", \"loop\": " << (keyframe->getLoop() ? "true" : "false")
                 << ", \"priority\": " << static_cast<S32>(keyframe->getPriority())
                 << ", \"exported\": " << (exported ? "true" : "false");
        if (exported) manifest << ", \"file\": \"" << filename << "\"";
        manifest << "}";
    }
    manifest << "\n  ]\n}\n";
}

bool export_cached_texture(const LLUUID& id, const std::string& texture_dir,
                           const std::string& texture_uri_dir, texture_map_t& textures)
{
    if (id.isNull()) return false;
    if (textures.find(id) != textures.end()) return true;

    LLViewerFetchedTexture* texture = gTextureList.findImage(id, TEX_LIST_STANDARD);
    if (!texture) return false;

    LLPointer<LLImageRaw> raw = texture->getRawImage();
    if (raw.isNull()) raw = texture->getSavedRawImage();
    if (raw.isNull())
    {
        // This only reads pixels already resident in VRAM; it does not request
        // or download a missing asset.
        texture->readbackRawImage();
        raw = texture->getRawImage();
    }
    if (raw.isNull() || raw->getWidth() <= 0 || raw->getHeight() <= 0) return false;

    LLPointer<LLImagePNG> png = new LLImagePNG;
    if (!png->encode(raw, 0.f)) return false;
    const std::string filename = id.asString() + ".png";
    const std::string full_path = gDirUtilp->add(texture_dir, filename);
    if (!png->save(full_path)) return false;

    ExportedTexture exported;
    exported.id = id;
    exported.uri = texture_uri_dir + "/" + filename;
    textures[id] = exported;
    return true;
}

void collect_material_textures(const LLViewerObject& object, const std::string& dae_filename,
                               texture_map_t& textures, std::vector<LLUUID>& diffuse_ids)
{
    const std::string uri_dir = "textures";
    const std::string texture_dir = gDirUtilp->add(gDirUtilp->getDirName(dae_filename), uri_dir);
    if (!LLFile::isdir(texture_dir)) LLFile::mkdir(texture_dir);

    diffuse_ids.resize(object.getNumTEs());
    for (S32 face = 0; face < object.getNumTEs(); ++face)
    {
        const LLTextureEntry* te = object.getTE(face);
        if (!te) continue;

        LLUUID diffuse_id = te->getID();
        if (const LLGLTFMaterial* pbr = te->getGLTFRenderMaterial())
        {
            if (pbr->mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR].notNull())
                diffuse_id = pbr->mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR];
            for (U32 channel = 0; channel < LLGLTFMaterial::GLTF_TEXTURE_INFO_COUNT; ++channel)
                export_cached_texture(pbr->mTextureId[channel], texture_dir, uri_dir, textures);
        }
        if (const LLMaterialPtr legacy = te->getMaterialParams())
        {
            export_cached_texture(legacy->getNormalID(), texture_dir, uri_dir, textures);
            export_cached_texture(legacy->getSpecularID(), texture_dir, uri_dir, textures);
        }
        if (export_cached_texture(diffuse_id, texture_dir, uri_dir, textures)) diffuse_ids[face] = diffuse_id;
    }
}

void write_materials(std::ostream& out, const LLViewerObject& object,
                     const texture_map_t& textures, const std::vector<LLUUID>& diffuse_ids)
{
    if (!textures.empty())
    {
        out << "  <library_images>\n";
        for (const auto& item : textures)
            out << "    <image id=\"image-" << item.first << "\" name=\"" << item.first
                << "\"><init_from>" << escape_xml(item.second.uri) << "</init_from></image>\n";
        out << "  </library_images>\n";
    }
    out << "  <library_effects>\n";
    for (S32 face = 0; face < object.getNumTEs(); ++face)
    {
        const LLTextureEntry* te = object.getTE(face);
        const LLColor4 color = te ? te->getColor() : LLColor4::white;
        out << "    <effect id=\"slot-" << face << "-fx\"><profile_COMMON>";
        const LLUUID diffuse_id = face < static_cast<S32>(diffuse_ids.size()) ? diffuse_ids[face] : LLUUID::null;
        if (textures.find(diffuse_id) != textures.end())
        {
            out << "<newparam sid=\"slot-" << face << "-surface\"><surface type=\"2D\"><init_from>image-"
                << diffuse_id << "</init_from></surface></newparam><newparam sid=\"slot-" << face
                << "-sampler\"><sampler2D><source>slot-" << face << "-surface</source></sampler2D></newparam>";
        }
        out << "<technique sid=\"common\"><phong><diffuse>";
        if (textures.find(diffuse_id) != textures.end())
            out << "<texture texture=\"slot-" << face << "-sampler\" texcoord=\"UVMap\"/>";
        else
            out << "<color>" << color.mV[0] << ' ' << color.mV[1] << ' ' << color.mV[2] << ' ' << color.mV[3] << "</color>";
        out << "</diffuse><transparent opaque=\"A_ONE\"><color>1 1 1 " << color.mV[3]
            << "</color></transparent><transparency><float>" << color.mV[3]
            << "</float></transparency></phong></technique></profile_COMMON>";
        if (te)
        {
            out << "<extra><technique profile=\"FIRESTORM_VIEWER_CACHE\"><texture_uuid>" << te->getID()
                << "</texture_uuid><scale>" << te->getScaleS() << ' ' << te->getScaleT()
                << "</scale><offset>" << te->getOffsetS() << ' ' << te->getOffsetT()
                << "</offset><rotation>" << te->getRotation() << "</rotation>";
            if (const LLMaterialPtr legacy = te->getMaterialParams())
                out << "<normal_uuid>" << legacy->getNormalID() << "</normal_uuid><specular_uuid>"
                    << legacy->getSpecularID() << "</specular_uuid>";
            if (const LLGLTFMaterial* pbr = te->getGLTFRenderMaterial())
            {
                out << "<pbr base_color_uuid=\"" << pbr->mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR]
                    << "\" normal_uuid=\"" << pbr->mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_NORMAL]
                    << "\" orm_uuid=\"" << pbr->mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS]
                    << "\" emissive_uuid=\"" << pbr->mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_EMISSIVE]
                    << "\" metallic=\"" << pbr->mMetallicFactor << "\" roughness=\"" << pbr->mRoughnessFactor << "\"/>";
            }
            out << "</technique></extra>";
        }
        out << "</effect>\n";
    }
    out << "  </library_effects>\n  <library_materials>\n";
    for (S32 face = 0; face < object.getNumTEs(); ++face)
    {
        out << "    <material id=\"slot-" << face << "\" name=\"Face " << face << "\"><instance_effect url=\"#slot-"
            << face << "-fx\"/></material>\n";
    }
    out << "  </library_materials>\n";
}

bool write_skin_controller(std::ostream& out, const LLVolume& volume, const LLMeshSkinInfo& skin, F32 unit_meter)
{
    if (skin.mJointNames.empty() || skin.mJointNames.size() != skin.mInvBindMatrix.size()) return false;
    const std::string id("viewer-cache-high-skin");
    std::vector<F32> weights;
    std::vector<S32> vcount;
    std::vector<S32> v;
    S32 vertices = 0;
    for (S32 face_index = 0; face_index < volume.getNumVolumeFaces(); ++face_index)
    {
        const LLVolumeFace& face = volume.getVolumeFace(face_index);
        if (!face_has_render_data(face)) continue;
        for (S32 vertex = 0; vertex < face.mNumVertices; ++vertex)
        {
            S32 influences = 0;
            if (face.mWeights)
            {
                const F32* packed = face.mWeights[vertex].getF32ptr();
                for (S32 influence = 0; influence < 4; ++influence)
                {
                    const S32 joint = static_cast<S32>(std::floor(packed[influence]));
                    const F32 weight = packed[influence] - std::floor(packed[influence]);
                    if (weight > 0.f && joint >= 0 && joint < static_cast<S32>(skin.mJointNames.size()))
                    {
                        v.push_back(joint);
                        v.push_back(static_cast<S32>(weights.size()));
                        weights.push_back(weight);
                        ++influences;
                    }
                }
            }
            vcount.push_back(influences);
            ++vertices;
        }
    }
    if (vertices == 0 || weights.empty()) return false;

    LLMatrix4 unit_scale;
    unit_scale.initScale(LLVector3(unit_meter, unit_meter, unit_meter));
    LLMatrix4 bind_shape(skin.mBindShapeMatrix.getF32ptr());
    bind_shape *= unit_scale;
    const LLMatrix4a bind_shape_meters(bind_shape);

    out << "  <library_controllers><controller id=\"" << id << "\"><skin source=\"#viewer-cache-high\"><bind_shape_matrix>";
    write_matrix(out, bind_shape_meters);
    out << "</bind_shape_matrix>\n    <source id=\"" << id << "-joints\"><Name_array id=\"" << id
        << "-joints-array\" count=\"" << skin.mJointNames.size() << "\">";
    for (const std::string& name : skin.mJointNames) out << escape_xml(name) << ' ';
    out << "</Name_array><technique_common><accessor source=\"#" << id << "-joints-array\" count=\"" << skin.mJointNames.size()
        << "\" stride=\"1\"><param name=\"JOINT\" type=\"Name\"/></accessor></technique_common></source>\n";
    out << "    <source id=\"" << id << "-bindposes\"><float_array id=\"" << id << "-bindposes-array\" count=\""
        << skin.mInvBindMatrix.size() * 16 << "\">";
    LLMatrix4 inverse_unit_scale;
    const F32 inverse_unit = 1.f / unit_meter;
    inverse_unit_scale.initScale(LLVector3(inverse_unit, inverse_unit, inverse_unit));
    for (const LLMatrix4a& matrix : skin.mInvBindMatrix)
    {
        LLMatrix4 inverse_bind_meters(inverse_unit_scale);
        inverse_bind_meters *= LLMatrix4(matrix.getF32ptr());
        write_matrix(out, LLMatrix4a(inverse_bind_meters));
    }
    out << "</float_array><technique_common><accessor source=\"#" << id << "-bindposes-array\" count=\""
        << skin.mInvBindMatrix.size() << "\" stride=\"16\"><param name=\"TRANSFORM\" type=\"float4x4\"/>"
        << "</accessor></technique_common></source>\n";
    write_float_source(out, id + "-weights", weights, 1, "W");
    out << "    <joints><input semantic=\"JOINT\" source=\"#" << id << "-joints\"/><input semantic=\"INV_BIND_MATRIX\" source=\"#"
        << id << "-bindposes\"/></joints><vertex_weights count=\"" << vertices << "\"><input semantic=\"JOINT\" source=\"#"
        << id << "-joints\" offset=\"0\"/><input semantic=\"WEIGHT\" source=\"#" << id << "-weights\" offset=\"1\"/><vcount>";
    for (S32 count : vcount) out << count << ' ';
    out << "</vcount><v>";
    for (S32 value : v) out << value << ' ';
    out << "</v></vertex_weights></skin></controller></library_controllers>\n";
    return true;
}

void notify_export(const std::string& notification, const std::string& object, const std::string& filename)
{
    LLSD args;
    args["OBJECT"] = object;
    args["FILENAME"] = filename;
    LLNotificationsUtil::add(notification, args);
}

void write_object_scale(std::ostream& out, const LLViewerObject& object)
{
    const LLVector3 scale = object.getScale();
    out << "<matrix sid=\"transform\">" << scale.mV[VX] << " 0 0 0 0 " << scale.mV[VY]
        << " 0 0 0 0 " << scale.mV[VZ] << " 0 0 0 0 1</matrix>";
}

S32 write_joint_hierarchy(std::ostream& out, const LLMeshSkinInfo& skin, F32 unit_meter)
{
    const S32 joint_count = static_cast<S32>(skin.mJointNames.size());
    std::map<std::string, S32> joint_indices;
    for (S32 index = 0; index < joint_count; ++index)
    {
        joint_indices[skin.mJointNames[index]] = index;
    }

    S32 root_index = 0;
    const auto pelvis = joint_indices.find("mPelvis");
    if (pelvis != joint_indices.end()) root_index = pelvis->second;

    std::vector<S32> parents(joint_count, -1);
    for (S32 index = 0; index < joint_count; ++index)
    {
        LLJoint* joint = gAgentAvatarp ? gAgentAvatarp->getJoint(skin.mJointNames[index]) : nullptr;
        for (LLJoint* parent = joint ? joint->getParent() : nullptr; parent; parent = parent->getParent())
        {
            const auto found = joint_indices.find(parent->getName());
            if (found != joint_indices.end())
            {
                parents[index] = found->second;
                break;
            }
        }
        // Keep a single COLLADA skeleton even if a legacy/custom joint cannot
        // be found in the current avatar hierarchy.  Its global bind pose still
        // produces the correct local transform relative to mPelvis.
        if (index != root_index && parents[index] < 0) parents[index] = root_index;
    }
    parents[root_index] = -1;

    std::vector<std::vector<S32>> children(joint_count);
    for (S32 index = 0; index < joint_count; ++index)
    {
        if (parents[index] >= 0) children[parents[index]].push_back(index);
    }

    std::function<void(S32, S32)> write_joint = [&](S32 index, S32 indent)
    {
        const std::string padding(indent, ' ');
        const std::string& name = skin.mJointNames[index];
        LLMatrix4 local_bind(skin.mInvBindMatrix[index].getF32ptr());
        local_bind.invert();
        if (parents[index] >= 0)
        {
            // LLMatrix4 uses row-vector transforms: global = local * parent.
            // Multiplying by the parent's inverse bind recovers the local pose.
            LLMatrix4 parent_inverse(skin.mInvBindMatrix[parents[index]].getF32ptr());
            local_bind *= parent_inverse;
        }
        else
        {
            // Bake the source length unit into the skeleton root.  Some DAE
            // importers (including Blender's current importer) ignore the
            // COLLADA asset unit metadata.
            LLMatrix4 unit_scale;
            unit_scale.initScale(LLVector3(unit_meter, unit_meter, unit_meter));
            local_bind *= unit_scale;
        }
        const LLMatrix4a local_bind_aligned(local_bind);

        out << padding << "<node id=\"joint-" << escape_xml(name) << "\" sid=\"" << escape_xml(name)
            << "\" name=\"" << escape_xml(name) << "\" type=\"JOINT\"><matrix sid=\"transform\">";
        write_matrix(out, local_bind_aligned);
        out << "</matrix>\n";
        for (S32 child : children[index]) write_joint(child, indent + 2);
        out << padding << "</node>\n";
    };
    write_joint(root_index, 4);
    return root_index;
}

void write_scene(std::ostream& out, const std::vector<S32>& lods, const LLMeshSkinInfo* skin,
                 const LLViewerObject& object, S32 material_count, F32 unit_meter)
{
    out << "  <library_visual_scenes><visual_scene id=\"Scene\" name=\"Scene\">\n";
    const bool high_loaded = std::find(lods.begin(), lods.end(), LLModel::LOD_HIGH) != lods.end();
    if (skin && high_loaded)
    {
        const S32 root_joint = write_joint_hierarchy(out, *skin, unit_meter);
        out << "    <node id=\"viewer-cache-high-node\">";
        // Rigged mesh vertices are positioned by bind shape + joint matrices.
        // Applying the in-world prim scale here would scale the garment twice.
        out << "<matrix sid=\"transform\">1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</matrix>";
        out << "<instance_controller url=\"#viewer-cache-high-skin\"><skeleton>#joint-"
            << escape_xml(skin->mJointNames[root_joint]) << "</skeleton><bind_material><technique_common>";
    }
    else if (high_loaded)
    {
        out << "    <node id=\"viewer-cache-high-node\">";
        write_object_scale(out, object);
        out << "<instance_geometry url=\"#viewer-cache-high\"><bind_material><technique_common>";
    }
    if (high_loaded)
    {
        for (S32 slot = 0; slot < material_count; ++slot)
        {
            out << "<instance_material symbol=\"slot-" << slot << "\" target=\"#slot-" << slot << "\"/>";
        }
        out << "</technique_common></bind_material>" << (skin ? "</instance_controller>" : "</instance_geometry>") << "</node>\n";
    }
    out << "  </visual_scene></library_visual_scenes><scene><instance_visual_scene url=\"#Scene\"/></scene>\n";
}

void save_reconstructed_mesh(const std::vector<std::string>& filenames, LLUUID object_id, std::string object_name)
{
    if (filenames.empty()) return;
    const std::string requested_filename = filenames.front();
    const std::string export_name = gDirUtilp->getBaseFileName(requested_filename, true);
    const std::string export_dir = gDirUtilp->add(gDirUtilp->getDirName(requested_filename), export_name + "_export");
    if (!LLFile::isdir(export_dir) && LLFile::mkdir(export_dir) != 0)
    {
        notify_export("ExportColladaFailure", object_name, requested_filename);
        return;
    }
    const std::string dae_filename = gDirUtilp->add(export_dir, export_name + ".dae");
    LLViewerObject* object = gObjectList.findObject(object_id);
    LLVOVolume* mesh = object ? dynamic_cast<LLVOVolume*>(object) : nullptr;
    if (!mesh || !mesh->isMesh() || !mesh->getVolume())
    {
        notify_export("ExportColladaFailure", object_name, dae_filename);
        return;
    }

    LLVolumeLODGroup* group = LLPrimitive::getVolumeManager()->getGroup(mesh->getVolume()->getParams());
    const LLMeshSkinInfo* mesh_skin = mesh->getSkinInfo();
    const bool valid_skin = mesh_skin && !mesh_skin->mJointNames.empty() &&
        mesh_skin->mJointNames.size() == mesh_skin->mInvBindMatrix.size();
    const F32 skin_unit_meter = valid_skin ? get_skin_unit_meter(*mesh_skin) : 1.f;
    std::ofstream out(dae_filename, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out.is_open())
    {
        notify_export("ExportColladaFailure", object_name, dae_filename);
        return;
    }
    out << std::setprecision(9);
    out << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<COLLADA version=\"1.4.1\" xmlns=\"http://www.collada.org/2005/11/COLLADASchema\">\n"
        << "  <asset><contributor><authoring_tool>Firestorm viewer cached mesh reconstructor</authoring_tool></contributor>"
        << "<unit name=\"meter\" meter=\"1\"/><up_axis>Z_UP</up_axis>"
        << "<extra><technique profile=\"FIRESTORM_VIEWER_CACHE\"><mesh_uuid>"
        << mesh->getMeshID() << "</mesh_uuid><note>Only LODs already decoded by this Viewer session are present.</note>"
        << "</technique></extra></asset>\n";
    texture_map_t textures;
    std::vector<LLUUID> diffuse_ids;
    collect_material_textures(*object, dae_filename, textures, diffuse_ids);
    write_materials(out, *object, textures, diffuse_ids);
    out << "  <library_geometries>\n";
    std::vector<S32> lods;
    S32 triangles = 0;
    for (S32 lod = FIRST_MESH_LOD; lod <= LAST_MESH_LOD; ++lod)
    {
        LLVolume* volume = group ? group->refLOD(lod) : nullptr;
        if (volume && volume->isMeshAssetLoaded())
        {
            const S32 count = write_geometry(out, *volume, *object, lod);
            if (count > 0)
            {
                lods.push_back(lod);
                triangles += count;
            }
        }
        if (group) group->derefLOD(volume);
    }
    out << "  </library_geometries>\n";
    const LLMeshSkinInfo* skin = nullptr;
    if (std::find(lods.begin(), lods.end(), LLModel::LOD_HIGH) != lods.end())
    {
        LLVolume* high = group ? group->refLOD(LLModel::LOD_HIGH) : nullptr;
        if (high && high->isMeshAssetLoaded() && mesh->getSkinInfo() &&
            write_skin_controller(out, *high, *mesh->getSkinInfo(), skin_unit_meter))
        {
            skin = mesh->getSkinInfo();
        }
        if (group) group->derefLOD(high);
    }
    write_scene(out, lods, skin, *object, object->getNumTEs(), skin_unit_meter);
    out << "</COLLADA>\n";
    out.close();
    if (triangles > 0 && out.good())
    {
        export_active_animations(dae_filename);
        LL_INFOS("MeshReconstructor") << "Wrote " << triangles << " cached mesh triangles to " << dae_filename
            << "; skin unit meter=" << skin_unit_meter << LL_ENDL;
        notify_export("ExportColladaSuccess", object_name, dae_filename);
    }
    else
    {
        notify_export("ExportColladaFailure", object_name, dae_filename);
    }
}
}

void FSMeshReconstructor::exportSelected()
{
    if (!verify_mesh_export_license())
    {
        LL_WARNS("MeshReconstructor") << "Mesh export license is missing, invalid, or belongs to another machine" << LL_ENDL;
        LLNotificationsUtil::add("FSMeshReconstructLicenseInvalid");
        return;
    }
    LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
    LLSelectNode* node = selection ? selection->getFirstRootNode() : nullptr;
    if (!node || !node->getObject())
    {
        LL_INFOS("MeshReconstructor") << "No object selection" << LL_ENDL;
        LLNotificationsUtil::add("FSMeshReconstructNoSelection");
        return;
    }

    LLVOVolume* mesh = nullptr;
    for (LLObjectSelection::iterator iter = selection->begin(); iter != selection->end() && !mesh; ++iter)
    {
        LLSelectNode* selected_node = *iter;
        mesh = selected_node ? find_decoded_mesh(selected_node->getObject()) : nullptr;
    }
    if (!mesh)
    {
        LLViewerObject* root = node->getObject()->getRootEdit();
        mesh = find_decoded_mesh(root ? root : node->getObject());
    }
    if (!mesh)
    {
        LLViewerObject* selected = node->getObject();
        LLVOVolume* selected_volume = dynamic_cast<LLVOVolume*>(selected);
        LL_INFOS("MeshReconstructor") << "No decoded Mesh in selected linkset; root=" << selected->getID()
            << " pcode=" << selected->getPCode() << " children=" << selected->getChildren().size()
            << " root_is_volume=" << (selected_volume != nullptr)
            << " root_is_mesh=" << (selected_volume && selected_volume->isMesh())
            << " root_has_volume=" << (selected_volume && selected_volume->getVolume()) << LL_ENDL;
        LLNotificationsUtil::add("FSMeshReconstructNotMesh");
        return;
    }

    const std::string object_name = node->mName.empty() ? "cached_mesh" : node->mName;
    LL_INFOS("MeshReconstructor") << "Using decoded Mesh " << mesh->getID() << " from selected linkset root "
        << node->getObject()->getID() << LL_ENDL;
    LLFilePickerReplyThread::startPicker(boost::bind(&save_reconstructed_mesh, _1, mesh->getID(), object_name),
        LLFilePicker::FFSAVE_COLLADA, LLDir::getScrubbedFileName(object_name + "_viewer_cache.dae"));
}
