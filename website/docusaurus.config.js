// @ts-check
// Docusaurus configuration for the Game & Watch Retro-Go — Experimental Lab site.
import {themes as prismThemes} from 'prism-react-renderer';

const ORG = 'jshsakura';
const PROJECT = 'game-and-watch-retro-go-sd';

/** @type {import('@docusaurus/types').Config} */
const config = {
  title: 'Game & Watch Retro-Go — Experimental Lab',
  tagline: 'A personal experimental fork: GBA, Super Metroid, a clock, and other rough experiments on the Nintendo Game & Watch.',
  favicon: 'img/favicon.svg',

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

  // Game & Watch design DNA — pixel display font + Korean-capable body font,
  // matching the companion game-and-what project.
  headTags: [
    {tagName: 'link', attributes: {rel: 'preconnect', href: 'https://fonts.googleapis.com'}},
    {tagName: 'link', attributes: {rel: 'preconnect', href: 'https://fonts.gstatic.com', crossorigin: 'anonymous'}},
  ],
  stylesheets: [
    'https://fonts.googleapis.com/css2?family=Press+Start+2P&family=Noto+Sans+KR:wght@400;500;700&display=swap',
  ],

  // Game & Watch edition switch (Zelda green ↔ Mario red), like game-and-what.
  clientModules: ['./src/clientModules/edition.js'],

  presets: [
    [
      'classic',
      /** @type {import('@docusaurus/preset-classic').Options} */
      ({
        docs: {
          sidebarPath: './sidebars.js',
          editUrl: `https://github.com/${ORG}/${PROJECT}/edit/testbed/website/`,
          showLastUpdateTime: true,
          showLastUpdateAuthor: true,
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

  // Free, offline, no-signup full-text search (indexes docs + devlog).
  themes: [
    [
      '@easyops-cn/docusaurus-search-local',
      {
        hashed: true,
        indexBlog: true,
        indexPages: true,
        docsRouteBasePath: '/docs',
        blogRouteBasePath: '/devlog',
        highlightSearchTermsOnTargetPage: true,
        searchResultLimits: 8,
      },
    ],
  ],

  themeConfig:
    /** @type {import('@docusaurus/preset-classic').ThemeConfig} */
    ({
      image: 'img/clock-hero.jpg',
      announcementBar: {
        id: 'experimental',
        content:
          '🧪 Experimental test builds — back up your SD card / saves before flashing.',
        backgroundColor: '#c9a227',
        textColor: '#1c150a',
        isCloseable: true,
      },
      // No light/dark toggle — the theme axis is the Zelda/Mario edition switch.
      colorMode: {
        defaultMode: 'dark',
        disableSwitch: true,
        respectPrefersColorScheme: false,
      },
      docs: {
        sidebar: {
          hideable: true,
          autoCollapseCategories: true,
        },
      },
      tableOfContents: {
        minHeadingLevel: 2,
        maxHeadingLevel: 4,
      },
      navbar: {
        title: 'G&W Retro-Go Lab',
        hideOnScroll: true,
        logo: {
          alt: 'Game & Watch pixel logo',
          src: 'img/favicon.svg',
        },
        items: [
          {
            type: 'docSidebar',
            sidebarId: 'docsSidebar',
            position: 'left',
            label: 'Docs',
          },
          {to: '/devlog', label: 'Devlog', position: 'left'},
          {
            href: 'https://github.com/sylverb/game-and-watch-retro-go-sd',
            label: '★ Upstream (sylverb)',
            position: 'right',
          },
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
        copyright: `A derivative, experimental fork — built with respect and gratitude on sylverb's retro-go-sd and the work of the retro-go contributors. GPLv2 · © ${2026}.`,
      },
      prism: {
        theme: prismThemes.github,
        darkTheme: prismThemes.dracula,
        additionalLanguages: ['bash', 'c', 'diff', 'makefile', 'ini', 'json'],
      },
    }),
};

export default config;
