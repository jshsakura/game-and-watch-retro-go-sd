// @ts-check

/** @type {import('@docusaurus/plugin-content-docs').SidebarsConfig} */
const sidebars = {
  docsSidebar: [
    'intro',
    {
      type: 'category',
      label: 'Systems',
      collapsed: false,
      items: ['systems', 'game-boy-advance', 'super-metroid'],
    },
    {
      type: 'category',
      label: 'Features',
      collapsed: false,
      items: ['features', 'overclock-and-power'],
    },
    {
      type: 'category',
      label: 'Building',
      collapsed: true,
      items: ['build-flags'],
    },
    'about',
  ],
};

export default sidebars;
