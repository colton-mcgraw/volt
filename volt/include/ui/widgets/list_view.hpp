#pragma once

#include <ui/painter.hpp>
#include <ui/widgets/row_view.hpp>

#include <cstddef>
#include <string>
#include <vector>

// -------------------------------------------------------------------------
// list_view, tree_view
// -------------------------------------------------------------------------
//
// The two row views (<ui/widgets/row_view.hpp>): a flat list of texts, and
// a tree of nodes that open and close.
//
// A tree shows the nodes whose ancestors are all expanded, as rows, each
// indented by its depth, with an arrow before the ones that have children.
// Clicking the arrow, or double-clicking the row, opens or closes a node;
// Right opens a node (or goes to its first child), Left closes it (or goes
// to its parent). The visible rows are kept as a flat list, rebuilt only
// when a node opens or closes, so painting is the row view's screenful.

namespace volt::ui
{

    class list_view : public row_view
    {
    public:
        explicit list_view(std::vector<std::string> items = {});

        const std::vector<std::string> &items() const noexcept { return m_items; }
        void set_items(std::vector<std::string> items);

        std::size_t row_count() const override { return m_items.size(); }

    protected:
        void paint_row(painter &p, std::size_t index, const rect &area) override;

    private:
        std::vector<std::string> m_items;
    };

    struct tree_node
    {
        std::string text;
        ui::icon icon = icon::none;
        std::vector<tree_node> children;
        bool expanded = false;

        // Whatever the application wants to find the node by.
        std::string id;
    };

    class tree_view : public row_view
    {
    public:
        explicit tree_view(std::vector<tree_node> roots = {});

        void set_roots(std::vector<tree_node> roots);
        const std::vector<tree_node> &roots() const noexcept { return m_roots; }

        std::size_t row_count() const override { return m_rows.size(); }

        // The node shown in row `index`, and how deep it is.
        tree_node &node(std::size_t index) const { return *m_rows[index].node; }
        int depth(std::size_t index) const { return m_rows[index].depth; }

        // The selected node, or null.
        tree_node *selected_node() const;

        void set_expanded(std::size_t index, bool expanded);
        void toggle(std::size_t index);
        void expand_all();

    protected:
        void paint_row(painter &p, std::size_t index, const rect &area) override;
        bool row_pressed(std::size_t index, const pointer_event &e) override;
        bool row_key(const key_event &e) override;
        bool on_click(const pointer_event &e) override;

    private:
        struct row
        {
            tree_node *node = nullptr;
            int depth = 0;
            int parent = -1; // row index of the parent
        };

        void rebuild();
        rect arrow_rect(std::size_t index) const;

        std::vector<tree_node> m_roots;
        std::vector<row> m_rows;
    };

} // namespace volt::ui
