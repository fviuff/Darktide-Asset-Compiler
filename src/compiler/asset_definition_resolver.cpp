#include "compiler/asset_definition_resolver.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace dtglb::compiler {
bool resolve_asset_definition(Scene& scene, std::string& error) {
    error.clear();
    scene.collider_primitives.clear();
    const auto& asset = scene.asset_definition;
    // a node actor's mesh is its shape, never drawn
    std::set<int> actor_nodes;
    std::set<std::string> actor_names;
    for (const auto& actor : asset.node_actors) {
        if (actor.source_node < 0 || static_cast<std::size_t>(actor.source_node) >= scene.nodes.size() ||
            !actor_nodes.insert(actor.source_node).second) {
            error = "node actor has an invalid or repeated node"; return false;
        }
        if (actor.name.empty() || !actor_names.insert(actor.name).second) {
            error = "actor names must be unique: '" + actor.name + "'"; return false;
        }
        if (scene.nodes[static_cast<std::size_t>(actor.source_node)].skin >= 0) {
            error = "actor '" + actor.name + "' is a skinned mesh; give it an unskinned mesh parented to the bone"; return false;
        }
    }
    if (!actor_nodes.empty()) {
        std::vector<Primitive> render;
        std::set<int> found;
        for (auto& primitive : scene.primitives) {
            if (!actor_nodes.count(primitive.collision_object)) { render.push_back(std::move(primitive)); continue; }
            if (primitive.mode != 4 || primitive.indices.size() < 3) {
                error = "actor meshes must be triangles"; return false;
            }
            found.insert(primitive.collision_object);
            scene.collider_primitives.push_back(std::move(primitive));
        }
        for (const auto& actor : asset.node_actors)
            if (!found.count(actor.source_node)) { error = "actor '" + actor.name + "' has no mesh to take its shape from"; return false; }
        scene.primitives = std::move(render);
    }
    if (!asset.node_bodies.empty() || !asset.joints.empty()) {
        if (asset.body) { error = "version 1 scene body cannot be mixed with version 2 node bodies"; return false; }
        std::map<std::string, const BodyDefinition*> bodies;
        std::set<std::string> ids;
        std::map<int, const BodyDefinition*> body_nodes;
        std::map<int, std::set<int>> used_skin_joints;
        for (const auto& primitive : scene.primitives) {
            if (primitive.source_node < 0 || static_cast<std::size_t>(primitive.source_node) >= scene.nodes.size()) continue;
            const int skin = scene.nodes[primitive.source_node].skin;
            if (skin >= 0 && static_cast<std::size_t>(skin) < scene.skins.size()) {
                auto& joints = used_skin_joints[skin];
                joints.insert(scene.skins[skin].joints.begin(), scene.skins[skin].joints.end());
            }
        }
        // A body on a helper node under a skin bone (a collider parented to a bone in
        // Blender) must move the bone itself, or the skinned mesh never follows it.
        // Retail ragdoll bodies are bound to their bones.
        const auto is_used_joint = [&](int node) {
            return std::any_of(used_skin_joints.begin(), used_skin_joints.end(),
                               [&](const auto& entry) { return entry.second.count(node) != 0; });
        };
        for (auto& body : scene.asset_definition.node_bodies) {
            if (body.source_node < 0 || static_cast<std::size_t>(body.source_node) >= scene.nodes.size() ||
                is_used_joint(body.source_node)) continue;
            const int parent = scene.nodes[static_cast<std::size_t>(body.source_node)].parent;
            if (parent < 0 || !is_used_joint(parent)) continue;
            scene.notes.push_back("body '" + body.id + "' on helper node '" + scene.nodes[static_cast<std::size_t>(body.source_node)].name +
                                  "' drives its parent bone '" + scene.nodes[static_cast<std::size_t>(parent)].name + "'");
            body.source_node = parent;
        }
        for (const auto& body : asset.node_bodies) {
            if (body.id.empty() || body.id.find('\0') != std::string::npos || !ids.insert(body.id).second || body.source_node < 0 || static_cast<std::size_t>(body.source_node) >= scene.nodes.size() ||
                !std::isfinite(body.mass) || body.mass < 0 || (body.actor != "static" && body.actor != "dynamic" && body.actor != "keyframed") ||
                (body.actor == "dynamic" && body.mass <= 0) || !body_nodes.emplace(body.source_node, &body).second ||
                (body.material != "default" && body.material != "iron" && body.material != "rubber") || scene.nodes[body.source_node].instance_attributes.size()) {
                error = "version 2 body has invalid fields, node, or GPU instancing"; return false;
            }
            std::size_t joint_skin_count = 0;
            for (const auto& [skin, joints] : used_skin_joints) {
                (void)skin;
                if (joints.count(body.source_node)) ++joint_skin_count;
            }
            if (joint_skin_count && (used_skin_joints.size() != 1 || joint_skin_count != 1)) {
                error = "version 2 skin-joint body binding requires one unambiguous used skin";
                return false;
            }
            bodies.emplace(body.id, &body);
        }
        std::map<int, const ColliderDefinition*> definitions;
        std::set<std::string> collider_ids;
        for (const auto& collider : asset.colliders) {
            if (collider.id.empty() || collider.source_node < 0 || static_cast<std::size_t>(collider.source_node) >= scene.nodes.size() ||
                !bodies.count(collider.body) || !definitions.emplace(collider.source_node, &collider).second ||
                !collider_ids.insert(collider.id).second ||
                (bodies.count(collider.id) && bodies.at(collider.id)->source_node != collider.source_node &&
                 bodies.at(collider.id)->source_node != scene.nodes[static_cast<std::size_t>(collider.source_node)].parent)) {
                error = "version 2 collider has an invalid body, node, or duplicate id"; return false;
            }
        }
        for (const auto& [id, body] : bodies) {
            bool found = false; for (const auto& collider : asset.colliders) if (collider.body == id) found = true;
            if (!found) { error = "version 2 body '" + id + "' has no collider"; return false; }
        }
        std::set<std::string> joint_ids;
        for (const auto& joint : asset.joints) {
            if (joint.id.empty() || joint.id.find('\0') != std::string::npos || !joint_ids.insert(joint.id).second ||
                joint.source_node < 0 || static_cast<std::size_t>(joint.source_node) >= scene.nodes.size() ||
                joint.body_a == joint.body_b || !bodies.count(joint.body_a) || !bodies.count(joint.body_b)) {
                error = "version 2 joint has duplicate/invalid body endpoints"; return false;
            }
            if (bodies.at(joint.body_a)->actor != "dynamic" && bodies.at(joint.body_b)->actor != "dynamic") {
                error = "authored joint requires a dynamic body endpoint"; return false;
            }
            if (joint.twist_min.has_value() != joint.twist_max.has_value()) {
                error = "authored joint requires both twist limits or neither"; return false;
            }
        }
        std::set<int> found;
        std::vector<Primitive> render;
        render.reserve(scene.primitives.size());
        for (auto& primitive : scene.primitives) {
            int owner = primitive.source_node;
            std::set<int> path;
            while (owner >= 0) {
                if (static_cast<std::size_t>(owner) >= scene.nodes.size()) { error = "invalid node ancestry in version 2 physics"; return false; }
                if (!path.insert(owner).second) { error = "cyclic node ancestry while resolving version 2 physics"; return false; }
                if (body_nodes.count(owner)) break;
                owner = scene.nodes[owner].parent;
            }
            const auto definition = definitions.find(primitive.collision_object);
            if (definition != definitions.end()) {
                const auto& collider = *definition->second;
                if (primitive.mode != 4 || primitive.indices.empty() || primitive.indices.size() % 3 != 0) { error = "version 2 collider must contain triangle geometry"; return false; }
                if (scene.nodes[collider.source_node].skin >= 0) { error = "version 2 collider is skinned"; return false; }
                found.insert(collider.source_node); scene.collider_primitives.push_back(primitive);
                if (collider.role == ColliderRole::Collision) continue;
                owner = bodies.at(collider.body)->source_node;
            }
            if (owner >= 0) {
                for (const auto& body : asset.node_bodies) if (body.source_node == owner) {
                    if (primitive.source_node >= 0 && scene.nodes[primitive.source_node].skin >= 0) { error = "version 2 body-owned render is skinned"; return false; }
                    primitive.render_owner_node = owner; break;
                }
            }
            render.push_back(std::move(primitive));
        }
        for (const auto& collider : asset.colliders) if (!found.count(collider.source_node)) { error = "version 2 collider has no emitted mesh"; return false; }
        scene.primitives = std::move(render);
        return true;
    }
    if (!asset.body && asset.colliders.empty()) return true;
    if (!asset.body) { error = "authored colliders require a body in the selected scene's darktide_asset extras"; return false; }
    const auto& body = *asset.body;
    if (body.id.empty() || body.id.find('\0') != std::string::npos ||
        !std::isfinite(body.mass) || body.mass < 0 ||
        (body.actor != "static" && body.actor != "dynamic" && body.actor != "keyframed") ||
        (body.material != "default" && body.material != "iron" && body.material != "rubber")) {
        error = "authored body requires a valid id, actor, mass, and physics material"; return false;
    }
    if (asset.colliders.empty()) { error = "authored body '" + body.id + "' has no collider nodes"; return false; }
    std::set<std::string> ids{body.id};
    std::map<int, const ColliderDefinition*> definitions;
    for (const auto& collider : asset.colliders) {
        if (collider.id.empty() || collider.id.find('\0') != std::string::npos || !ids.insert(collider.id).second ||
            collider.body != body.id || collider.source_node < 0 ||
            static_cast<std::size_t>(collider.source_node) >= scene.nodes.size() ||
            !definitions.emplace(collider.source_node, &collider).second) {
            error = "collider '" + collider.id + "' has an invalid/duplicate id, node, or body reference"; return false;
        }
    }
    std::set<int> found;
    std::vector<Primitive> render;
    render.reserve(scene.primitives.size());
    for (auto& primitive : scene.primitives) {
        const auto definition = definitions.find(primitive.collision_object);
        if (definition == definitions.end()) { render.push_back(std::move(primitive)); continue; }
        const auto& collider = *definition->second;
        if (primitive.mode != 4 || primitive.indices.empty() || primitive.indices.size() % 3 != 0) {
            error = "collider '" + collider.id + "' must contain triangle geometry"; return false;
        }
        if (scene.nodes[collider.source_node].skin >= 0) {
            error = "collider '" + collider.id + "' is skinned; author a rigid collision mesh for this body"; return false;
        }
        found.insert(collider.source_node);
        scene.collider_primitives.push_back(primitive);
        if (collider.role == ColliderRole::Both) render.push_back(std::move(primitive));
    }
    for (const auto& collider : asset.colliders) {
        if (!found.count(collider.source_node)) {
            error = "collider '" + collider.id + "' has no emitted mesh in the selected visible scene"; return false;
        }
    }
    scene.primitives = std::move(render);
    return true;
}
}
