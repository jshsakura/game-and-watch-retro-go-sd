// @ts-check
// Docusaurus configuration for the Game & Watch Retro-Go — Experimental Lab site.
import {themes as prismThemes} from 'prism-react-renderer';

const ORG = 'jshsakura';
const PROJECT = 'game-and-watch-retro-go-sd';

/** @type {import('@docusaurus/types').Config} */
const config = {
  title: 'Game & Watch Retro-Go — Experimental Lab',
  tagline: 'A personal experimental fork: GBA, Super Metroid, a clock, and other rough experiments on the Nintendo Game & Watch.',
  favicon: 'img/favicon.png',

  url: `https://${ORG}.github.io`,
  baseUrl: `/${PROJECT}/`,

  organizationName: ORG,
  projectName: PROJECT,
  deploymentBranch: 'gh-pages',
  trailingSlash: false,

  // Cross-links between ported docs are still being tidied — warn, don't fail the build.
  onBrokenLinks: 'warn',

  markdown: {
    hooks: {
      onBrokenMarkdownLinks: 'warn',
    },
  },

  i18n: {
    defaultLocale: 'en',
    locales: ['en'],
  },

  presets: [
    [
      'classic',
      /** @type {import('@docusaurus/preset-classic').Options} */
      ({
        docs: {
          sidebarPath: './sidebars.js',
          editUrl: `https://github.com/${ORG}/${PROJECT}/edit/testbed/website/`,
        },
        blog: {
          path: 'blog',
          routeBasePath: 'devlog',
          blogTitle: 'Devlog',
          blogDescription: 'Development journal for the Game & Watch Retro-Go experimental lab fork.',
          blogSidebarTitle: 'Recent entries',
          blogSidebarCount: 'ALL',
          showReadingTime: true,
          feedOptions: {
            type: ['rss', 'atom'],
            title: 'G&W Retro-Go Lab — Devlog',
            xslt: true,
          },
          onInlineTags: 'warn',
          onInlineAuthors: 'warn',
          onUntruncatedBlogPosts: 'ignore',
          editUrl: `https://github.com/${ORG}/${PROJECT}/edit/testbed/website/`,
        },
        theme: {
          customCss: './src/css/custom.css',
        },
      }),
    ],
  ],

  themeConfig:
    /** @type {import('@docusaurus/preset-classic').ThemeConfig} */
    ({
      image: 'img/clock-hero.jpg',
      colorMode: {
        respectPrefersColorScheme: true,
      },
      navbar: {
        title: 'G&W Retro-Go Lab',
        items: [
          {
            type: 'docSidebar',
            sidebarId: 'docsSidebar',
            position: 'left',
            label: 'Docs',
          },
          {to: '/devlog', label: 'Devlog', position: 'left'},
          {
            href: `https://github.com/${ORG}/${PROJECT}/releases`,
            label: 'Releases',
            position: 'right',
          },
          {
            href: `https://github.com/${ORG}/${PROJECT}`,
            label: 'GitHub',
            position: 'right',
          },
        ],
      },
      footer: {
        style: 'dark',
        links: [
          {
            title: 'Docs',
            items: [
              {label: 'Overview', to: '/docs/intro'},
              {label: 'Supported systems', to: '/docs/systems'},
              {label: 'Game Boy Advance', to: '/docs/game-boy-advance'},
              {label: 'Overclock & power', to: '/docs/overclock-and-power'},
            ],
          },
          {
            title: 'Project',
            items: [
              {label: 'Devlog', to: '/devlog'},
              {label: 'About / credits', to: '/docs/about'},
              {label: 'Releases', href: `https://github.com/${ORG}/${PROJECT}/releases`},
            ],
          },
          {
            title: 'Upstream',
            items: [
              {label: 'sylverb/game-and-watch-retro-go-sd', href: 'https://github.com/sylverb/game-and-watch-retro-go-sd'},
              {label: 'Companion: game-and-what', href: 'https://github.com/jshsakura/game-and-what'},
            ],
          },
        ],
        copyright: `Experimental lab fork · built on sylverb's retro-go-sd · GPLv2. Copyright © ${2026}.`,
      },
      prism: {
        theme: prismThemes.github,
        darkTheme: prismThemes.dracula,
      },
    }),
};

export default config;
